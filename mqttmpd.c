#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <getopt.h>
#include <syslog.h>
#include <mosquitto.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/in.h>

#define NAME "mqttmpd"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* generic error logging */
#define mylog(loglevel, fmt, ...) \
	({\
		syslog(loglevel, fmt, ##__VA_ARGS__); \
		if (loglevel <= LOG_ERR)\
			exit(1);\
	})
#define ESTR(num)	strerror(num)

/* program options */
static const char help_msg[] =
	NAME ": an MQTT-to-MPD control bridge\n"
	"usage:	" NAME " [OPTIONS ...] [TOPIC]\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -p, --mpd=HOST[:PORT] Specify alternate MPD host+port\n"
	"\n"
	"Paramteres\n"
	" TOPIC		The root topic to publish MPD topics\n"
	"		Default: mpd\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "mqtt", required_argument, NULL, 'm', },
	{ "mpd", required_argument, NULL, 'p', },
	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:p:";

/* signal handler */
static volatile int sigterm;

static void onsigterm(int sig)
{
	sigterm = 1;
}

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;
static const char *topicroot = "mpd";
static int topicrootlen = 3;

/* MPD parameters */
static const char *mpd_host = "localhost";
static int mpd_port = 6600;

/* state */
static struct mosquitto *mosq;
static int mpdsock;
static char recvbuf[1024*16];
/* MPD is 'idle' */
static int mpdidle;
/* Keep number of outstanding cmd's */
static int mpdncmds;

/* Keep last requested playlist */
static char *lastreqplaylist;

/* random playlist chooser data */
static char **pltable;
static int pltablesize;
static int pltablefill;
static int pltablelisten;

/* MQTT iface */
static void my_mqtt_log(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
	static const int logpri_map[] = {
		MOSQ_LOG_ERR, LOG_ERR,
		MOSQ_LOG_WARNING, LOG_WARNING,
		MOSQ_LOG_NOTICE, LOG_NOTICE,
		MOSQ_LOG_INFO, LOG_INFO,
		MOSQ_LOG_DEBUG, LOG_DEBUG,
		0,
	};
	int j;

	for (j = 0; logpri_map[j]; j += 2) {
		if (level & logpri_map[j]) {
			mylog(logpri_map[j+1], "[mosquitto] %s", str);
			return;
		}
	}
}

static void playlist_switched(const char *value)
{
	int ret;
	char *topic;

	if (lastreqplaylist && strcmp(value ?: "", lastreqplaylist)) {
		asprintf(&topic, "%s/playlists/%s", topicroot, lastreqplaylist);
		ret = mosquitto_publish(mosq, NULL, topic, 1, "0", mqtt_qos, 1);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_publish %s: %s", topic, mosquitto_strerror(ret));
		free(topic);
		free(lastreqplaylist);
		lastreqplaylist = NULL;
	}
	if (!value)
		return;
	lastreqplaylist = strdup(value);
	asprintf(&topic, "%s/playlists/%s", topicroot, value);
	ret = mosquitto_publish(mosq, NULL, topic, 1, "1", mqtt_qos, 1);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", topic, mosquitto_strerror(ret));
	free(topic);
}

#define send_mpd(sock, fmt, ...) \
	sendto_mpd(sock, fmt ";status;currentsong;outputs", ##__VA_ARGS__)
static int sendto_mpd(int sock, const char *fmt, ...)
{
	int ret;
	char *str, *str2;
	va_list va;

	va_start(va, fmt);
	vasprintf(&str2, fmt,va);
	va_end(va);
	asprintf(&str, "%scommand_list_begin;%s;idle;command_list_end;", mpdidle ? "noidle;" : "", str2);
	free(str2);
	/* replace ; with \n */
	for (str2 = str; *str2; ++str2)
		if (*str2 == ';')
			*str2 = '\n';

	ret = send(sock, str, strlen(str), MSG_NOSIGNAL);
	if (ret < 0)
		mylog(LOG_ERR, "mpd send '%s' failed: %s", str, ESTR(errno));
	free(str);
	++mpdncmds;
	mpdidle = 1;
	return ret;
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	char *subtopic, *value;

	if (msg->retain)
		/* ignore retained msgs */
		return;
	/* we don't receive any messsages outside topicroot/ */
	subtopic = msg->topic + topicrootlen + 1;
	value = msg->payload ?: "";

	if (!strcmp(subtopic, "play/set")) {
		if (!strcmp("0", value))
			value = "pause 1";
		else if (!strcmp("1", value))
			value = "play";
		send_mpd(mpdsock, "%s", value);

	} else if (!strcmp(subtopic, "next")) {
		if (!strcmp(value, "1"))
			send_mpd(mpdsock, "next");

	} else if (!strcmp(subtopic, "previous")) {
		if (!strcmp(value, "1"))
			send_mpd(mpdsock, "previous");

	} else if (!strcmp(subtopic, "clear")) {
		if (!strcmp(value, "1"))
			send_mpd(mpdsock, "clear");

	} else if (!strcmp(subtopic, "stop")) {
		if (!strcmp(value, "1"))
			send_mpd(mpdsock, "stop");

	} else if (!strcmp(subtopic, "shuffle")) {
		if (!strcmp(value, "1"))
			send_mpd(mpdsock, "shuffle");

	} else if (!strcmp(subtopic, "play/ctrl")) {
		static const char *cmds[] = {
			"next", "previous",
			"stop",
			"clear", "shuffle",
			NULL,
		};
		int j;

		for (j = 0; cmds[j]; ++j) {
			if (!strcmp(value, cmds[j])) {
				send_mpd(mpdsock, "%s", cmds[j]);
				break;
			}
		}
	} else if (!strcmp(subtopic, "volume/set"))
		send_mpd(mpdsock, "setvol %.0lf", strtod(value, NULL)*100);

	else if (!strcmp(subtopic, "random/set"))
		send_mpd(mpdsock, "random %s", value);

	else if (!strcmp(subtopic, "consume/set"))
		send_mpd(mpdsock, "consume %s", value);

	else if (!strcmp(subtopic, "repeat/set"))
		send_mpd(mpdsock, "repeat %s", value);

	else if (!strcmp(subtopic, "playlist/choose1")) {
		/* issue list-playlist */
		send_mpd(mpdsock, "listplaylist %s;status", value);
		/* reset table */
		pltablefill = 0;
		pltablelisten = 1;

	} else if (!strcmp(subtopic, "choose1")) {
		/* issue list-playlist */
		send_mpd(mpdsock, "list file %s;status", value);
		/* reset table */
		pltablefill = 0;
		pltablelisten = 1;

	} else if (!strcmp(subtopic, "playlist/set")) {
playlist:
		send_mpd(mpdsock, "clear;load %s;play", value);
		playlist_switched(value);
	}

	else if (!strncmp(subtopic, "playlist/", 9)) {
		if (!strcmp("0", value)) {
			send_mpd(mpdsock, "pause 1");
			return;
		}
		value = subtopic+9;
		/* this is just a different way of loading a playlist,
		 * goto is justified
		 */
		if (!strchr(value, '/'))
			/* only treat a playlist without any subtopics */
			goto playlist;

	} else
		;//mylog(LOG_WARNING, "Unhandled subtopic '%s=%s'", subtopic, value);
}

static int connect_uri(const char *host, int port, int preferred_type)
{
	int sock;

	if (*host == '@' || *host == '/') {
		/* unix socket */
		struct sockaddr_un addr = {
			.sun_family = AF_UNIX,
		};
		int socklen = sizeof(addr);

		strcpy(addr.sun_path, host);
		if (*host == '@')
			addr.sun_path[0] = 0;
		else
			socklen = strlen(host) + offsetof(struct sockaddr_un, sun_path);
		sock = socket(AF_UNIX, preferred_type, 0);
		if (sock < 0)
			mylog(LOG_ERR, "socket AF_UNIX ...: %s", ESTR(errno));
		if (connect(sock, (void *)&addr, socklen) < 0)
			mylog(LOG_ERR, "bind %s: %s", host, ESTR(errno));
		return sock;
	}

	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = preferred_type,
		.ai_protocol = 0,
		.ai_flags = 0,
	}, *paddr = NULL, *ai;
	char portstr[32];

	sprintf(portstr, "%u", port);
#ifdef AI_NUMERICSERV
	hints.ai_flags |= AI_NUMERICSERV;
#endif
	/* resolve host to IP */
	if (getaddrinfo(host, portstr, &hints, &paddr) < 0) {
		mylog(LOG_WARNING, "getaddrinfo %s %s: %s", host, portstr, ESTR(errno));
		return -1;
	}
	/* create socket */
	for (ai = paddr; ai; ai = ai->ai_next) {
		sock = socket(ai->ai_family, ai->ai_socktype,
				ai->ai_protocol);
		if (sock < 0)
			continue;
		if (connect(sock, ai->ai_addr, ai->ai_addrlen) >= 0)
			/* success */
			break;
		close(sock);
	}
	if (!ai) {
		/* no more addrinfo left over */
		sock = -1;
		mylog(LOG_WARNING, "connect %s:%u failed: %s", host, port, ESTR(errno));
	}
	freeaddrinfo(paddr);
	return sock;
}

static const char *const changes[] = {
	"player",
	"mixer",
	"options",
	"output",
	"playlist",
	NULL,
};

static const char *const hideprops[] = {
	"changed",
	NULL,
};

static int strpresent(const char *str, const char *const table[])
{
	for (; *table; ++table)
		if (!strcmp(*table, str))
			return 1;
	return 0;
}

static char *propvalue(char *str)
{
	char *pos;

	pos = strstr(str, ": ");
	if (!pos)
		return NULL;
	*pos = 0;
	return pos+2;
}

/* remember mpd state, in [x+0]=key, [x+1]=<value>, ... */
static char **state;
static int nstate, sstate; /* used vs. allocated */

static char **propcache(const char *propname)
{
	int j;

	for (j = 0; j < nstate; j += 2) {
		if (!strcmp(propname, state[j]))
			return state+j+1;
	}
	if ((nstate + 2) > sstate) {
		sstate += 128;
		state = realloc(state, sstate*sizeof(state[0]));
		if (!state)
			mylog(LOG_ERR, "realloc state: %s", ESTR(errno));
	}
	state[j] = strdup(propname);
	/* pre-assign a default value, and avoid multiple checks
	 * for the presence of a value
	 */
	state[j+1] = strdup("");
	nstate += 2;
	return state+j+1;
}

int main(int argc, char *argv[])
{
	int opt, ret;
	char *str;
	char mqtt_name[32];
	int logmask = LOG_UPTO(LOG_NOTICE);
	struct pollfd pf[2] = {};

	/* argument parsing */
	while ((opt = getopt_long(argc, argv, optstring, long_opts, NULL)) >= 0)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s %s\nCompiled on %s %s\n",
				NAME, VERSION, __DATE__, __TIME__);
		exit(0);
	case 'v':
		switch (logmask) {
		case LOG_UPTO(LOG_NOTICE):
			logmask = LOG_UPTO(LOG_INFO);
			break;
		case LOG_UPTO(LOG_INFO):
			logmask = LOG_UPTO(LOG_DEBUG);
			break;
		}
		break;
	case 'm':
		mqtt_host = optarg;
		str = strrchr(optarg, ':');
		if (str > mqtt_host && *(str-1) != ']') {
			/* TCP port provided */
			*str = 0;
			mqtt_port = strtoul(str+1, NULL, 10);
		}
		break;
	case 'p':
		mpd_host = optarg;
		break;

	default:
		fprintf(stderr, "unknown option '%c'", opt);
	case '?':
		fputs(help_msg, stderr);
		exit(1);
		break;
	}

	if (optind < argc)
		topicroot = argv[optind];
	topicrootlen = strlen(topicroot);

	openlog(NAME, LOG_PERROR, LOG_LOCAL2);
	setlogmask(logmask);

	signal(SIGTERM, onsigterm);
	signal(SIGINT, onsigterm);

	/* connect to MPD */
	mpdsock = connect_uri(mpd_host, mpd_port, SOCK_STREAM);
	if (mpdsock < 0)
		exit(1);

	/* MQTT start */
	mosquitto_lib_init();
	sprintf(mqtt_name, "%s-%i", NAME, getpid());
	mosq = mosquitto_new(mqtt_name, true, 0);
	if (!mosq)
		mylog(LOG_ERR, "mosquitto_new failed: %s", ESTR(errno));
	/* mosquitto_will_set(mosq, "TOPIC", 0, NULL, mqtt_qos, 1); */

	mosquitto_log_callback_set(mosq, my_mqtt_log);
	mosquitto_message_callback_set(mosq, my_mqtt_msg);

	ret = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
	if (ret)
		mylog(LOG_ERR, "mosquitto_connect %s:%i: %s", mqtt_host, mqtt_port, mosquitto_strerror(ret));

	/* SUBSCRIBE */
	asprintf(&str, "%s/#", topicroot);
	ret = mosquitto_subscribe(mosq, NULL, str, mqtt_qos);
	if (ret)
		mylog(LOG_ERR, "mosquitto_subscribe '%s': %s", str, mosquitto_strerror(ret));
	free(str);

	/* prepare poll */
	pf[0].fd = mosquitto_socket(mosq);
	pf[0].events = POLLIN;
	pf[1].fd = mpdsock;
	pf[1].events = POLLIN;

	mpdncmds = 1; /* expect 'OK MPD ... */

	for (; !sigterm;) {
		/* mosquitto things to do each iteration */
		ret = mosquitto_loop_misc(mosq);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop_misc: %s", mosquitto_strerror(ret));
		if (mosquitto_want_write(mosq)) {
			ret = mosquitto_loop_write(mosq, 1);
			if (ret)
				mylog(LOG_ERR, "mosquitto_loop_write: %s", mosquitto_strerror(ret));
		}
		/* prepare wait */
		ret = poll(pf, sizeof(pf)/sizeof(pf[0]), 1000);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0)
			mylog(LOG_ERR, "poll ...");

		if (pf[0].revents) {
			/* mqtt read ... */
			ret = mosquitto_loop_read(mosq, 1);
			if (ret)
				mylog(LOG_ERR, "mosquitto_loop_read: %s", mosquitto_strerror(ret));
		}
		if (pf[1].revents) {
			char *tok, *topic, *saved, *value, **pcache;
			char valbuf[32];
			__attribute__((unused))
			char *outputname, outputid[32];

			/* read mpd changes */
			ret = recv(mpdsock, recvbuf, sizeof(recvbuf)-1, 0);
			if (ret < 0)
				mylog(LOG_ERR, "recv mpd: %s", ESTR(errno));
			if (ret == 0)
				mylog(LOG_ERR, "recv mpd: closed");

			for (tok = strtok_r(recvbuf, "\n\r", &saved); tok; tok = strtok_r(NULL, "\n\r", &saved)) {
				if (!strncmp(tok, "OK", 2) || !strncmp(tok, "ACK", 3)) {
					if (!strncmp(tok, "ACK", 3))
						mylog(LOG_WARNING, "%s", tok);
					/* command returned */
					mpdncmds -= 1;
					mylog(LOG_INFO, "< '%s'", tok);
					continue;
				}
				value = propvalue(tok);
				if (!value)
					continue;

				if (pltablefill && strcmp(tok, "file")) {
					/* playlist request, ended,
					 * and something else received */
					int idx;
					srand48(time(NULL));
					idx = drand48()*pltablefill;
					send_mpd(mpdsock, "clear;add %s;play", pltable[idx]);
					pltablefill = 0;
					/* stop recording files */
					pltablelisten = 0;
				}
				/* replace 'state' */
				if (!strcmp(tok, "state")) {
					mylog(LOG_INFO, "< '%s: %s'", tok, value);
					tok = "play";
					value = !strcmp(value, "play") ? "1" : "0";
					if (*value == '0')
						playlist_switched(NULL);
				} else if (!strcmp(tok, "volume")) {
					sprintf(valbuf, "%.2lf", strtoul(value, 0, 10)/100.0);
					value = valbuf;
				}
				if (strpresent(tok, hideprops))
					continue;
				if (!strcmp(tok, "outputid")) {
					sprintf(outputid, "output%s", value);
					continue;
				} else if (!strcmp(tok, "outputname")) {
					outputname = value;
					continue;
				} else if (!strcmp(tok, "outputenabled")) {
					tok = outputid;
				} else if (pltablelisten && !strcmp(tok, "file")) {
					/* element in playlist info */
					if (pltablefill >= pltablesize) {
						pltablesize += 16;
						pltable = realloc(pltable, pltablesize*sizeof(*pltable));
						if (!pltable)
							mylog(LOG_ERR, "realloc failed: %s", ESTR(errno));
						memset(pltable+pltablefill, 0, (pltablesize-pltablefill)*sizeof(*pltable));
					}
					if (pltable[pltablefill])
						free(pltable[pltablefill]);
					pltable[pltablefill++] = strdup(value);
					continue;
				}

				pcache = propcache(tok);
				if (strcmp(value, *pcache)) {
					free(*pcache);
					*pcache = strdup(value);
					/* publish */
					asprintf(&topic, "%s/%s", topicroot, tok);
					ret = mosquitto_publish(mosq, NULL, topic, strlen(value), value, mqtt_qos, 1);
					if (ret < 0)
						mylog(LOG_ERR, "mosquitto_publish %s: %s", topic, mosquitto_strerror(ret));
					free(topic);
				}
			}

			if (!mpdncmds)
				/* schedule new data retrieve */
				sendto_mpd(mpdsock, "status;currentsong;outputs");
		}
	}

	/* destruct, enable for memory debugging */
	int j;
	for (j = 0; j < nstate; ++j)
		free(state[j]);
	if (state)
		free(state);

	mosquitto_disconnect(mosq);
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
	return 0;
}
