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

#include "lib/liburi.h"
#include "lib/libnetrc.h"

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
	" -m, --mqtt=[user[:pass]@]HOST[:PORT] Specify alternate MQTT user,pass,host,port\n"
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
static const char *mqtt_uri = "localhost";
static int mqtt_default_port = 1883;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;
static const char *topicroot = "mpd";
static int topicrootlen = 3;

/* MPD parameters */
static const char *mpd_uri = "localhost";
static int mpd_default_port = 6600;

/* state */
static struct mosquitto *mosq;
static int mpdsock;
static char recvbuf[1024*16];
/* MPD is 'idle' */
static int mpdidle;
/* Keep number of outstanding cmd's */
static int mpdncmds;

/* random playlist chooser data */
static char **pltable;
static int pltablesize;
static int pltablefill;
static int pltablelisten;
static int pltableaction;
#define TABLE_CHOOSE1	0
#define TABLE_PLAY	1

/* iterate playlist state */
static char *plsel;
static time_t plselt;
/* indicate if plsel was selected from a list, or single entry */
static int plselmulti;

static int playing;
static time_t playingt;

/* MQTT iface */
static void my_mqtt_log(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
	static const int logpri_map[] = {
		MOSQ_LOG_ERR, LOG_ERR,
		MOSQ_LOG_WARNING, LOG_WARNING,
		MOSQ_LOG_NOTICE, LOG_NOTICE,
		MOSQ_LOG_INFO, LOG_INFO,
		//MOSQ_LOG_DEBUG, LOG_DEBUG,
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

__attribute__((format(printf,3,4)))
static void mymqttpub(const char *topic, int retain, const char *fmt, ...)
{
	int ret;
	va_list va;
	char *fulltopic, *payload = NULL;

	if (fmt) {
		va_start(va, fmt);
		vasprintf(&payload, fmt, va);
		va_end(va);
	}

	asprintf(&fulltopic, "%s/%s", topicroot, topic);

	ret = mosquitto_publish(mosq, NULL, fulltopic, strlen(payload ?: ""), payload, mqtt_qos, retain);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", fulltopic, mosquitto_strerror(ret));
	free(fulltopic);
	free(payload);
}

static int sendto_mpd_raw(int sock, const char *str)
{
	int ret;

	ret = send(sock, str, strlen(str), MSG_NOSIGNAL);
	if (ret < 0) {
		mylog(LOG_ERR, "mpd send '%s' failed: %s", str, ESTR(errno));
		exit(1);
	}
	return ret;
}
static inline int sendto_mpd_pre(int sock)
{
	if (mpdidle)
		sendto_mpd_raw(sock, "noidle\n");
	mpdidle = 0;
	return sendto_mpd_raw(sock, "command_list_begin\n");
}

static inline int sendto_mpd_post(int sock)
{
	++mpdncmds;
	mpdidle = 1;
	return sendto_mpd_raw(sock, "idle\ncommand_list_end\n");
}
static int sendto_mpd_direct(int sock, const char *fmt, ...)
{
	va_list va;
	char *str;
	int ret;

	va_start(va, fmt);
	vasprintf(&str, fmt,va);
	va_end(va);
	ret = sendto_mpd_raw(sock, str);
	free(str);
	return ret;
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
	mylog(LOG_INFO, "> '%s'", str);
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

static const char *modifiers_to_cmds(const char *mods)
{
	static char buf[1024];
	char *str;

	if (!mods)
		return "";
	str = buf;
	*str = 0;
	for (; *mods; ++mods)
	switch (*mods) {
	case 'c':
	case 'C':
		str += sprintf(str, ";consume %i", !isupper(*mods));
		break;
	case 'z':
	case 'Z':
		str += sprintf(str, ";random %i", !isupper(*mods));
		break;
	case 'r':
	case 'R':
		str += sprintf(str, ";repeat %i", !isupper(*mods));
		break;
	case 's':
		str += sprintf(str, ";shuffle");
		break;
	}
	return buf;
}

char *strtokquote(char *str, const char *sep)
{
	static char *next;
	char *start, *dst;
	int quoted = 0;
	int escaped = 0;

	if (str)
		next = str;
	if (!next)
		return NULL;

	for (start = dst = next; *next; ++next) {
		if (escaped) {
			*dst++ = *next;
			escaped = 0;

		} else if (*next == '\\') {
			escaped = 1;

		} else if (*next == '"') {
			quoted = !quoted;

		} else if (quoted) {
			*dst++ = *next;

		} else if (!strchr(sep, *next)) {
			*dst++ = *next;

		} else if (dst > start) {
			/* terminate if we have seperators after token */
			break;
		}
	}
	/* skip next seperator */
	for (; *next && strchr(sep, *next); ++next);

	if (!*next)
		next = NULL;
	/* null-terminate un-escaped string
	 * it can be equal to escaped string */
	*dst = 0;
	return start;
}

static char **tokenize(char *str, const char *sep)
{
	char **a = NULL;
	int n = 0, s = 0;
	char *tok;

	for (tok = strtokquote(str, sep); tok; tok = strtokquote(NULL, sep)) {
		if (n+1 >= s) {
			s += 16;
			a = realloc(a, sizeof(*a)*s);
			if (!a)
				mylog(LOG_ERR, "realloc %u char *: %s", s, ESTR(errno));
		}
		a[n++] = tok;
	}
	a[n] = NULL;
	return a;
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

        } else if (!strcmp(subtopic, "toggle")) {
                        send_mpd(mpdsock, "pause");
 
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
		pltableaction = TABLE_CHOOSE1;

	} else if (!strcmp(subtopic, "choose1")) {
		/* issue list-playlist */
		send_mpd(mpdsock, "list file %s;status", value);
		/* reset table */
		pltablefill = 0;
		pltablelisten = 1;
		pltableaction = TABLE_CHOOSE1;

	} else if (!strcmp(subtopic, "playlist/select")) {
		time_t now;
		char **pls;

		time(&now);

		pls = tokenize(value, " \t");
		if (!pls || !*pls) {
			if (pls)
				free(pls);
			mylog(LOG_WARNING, "empty playlist selection provide");
			return;
		}
		if (plsel && !playing && (now - playingt) > 10) {
			/* clear plsel cache if mpd stopped more than 10s ago */
			free(plsel);
			plsel = NULL;
		}
		if (pls[1]) {
			/* >1 items provided */
			char **it;
			if (!plsel && playing) {
				/* currently playing, but outside playlist/select */
				mylog(LOG_NOTICE, "stop playing");
				send_mpd(mpdsock, "stop");
				mymqttpub("playlist/selected", 0, NULL);
				return;
			}
			if ((now - plselt) > 10 && plsel) {
				/* stop last selected playing song */
				mylog(LOG_NOTICE, "stop playlist '%s'", plsel);
				free(plsel);
				plsel = NULL;
				mymqttpub("playlist/selected", 0, NULL);
				send_mpd(mpdsock, "stop");
				return;
			}

			/* lookup last playlist */
			for (it = pls; *it; ++it)
				if (!strcmp(*it, plsel ?: ""))
					break;
			if (*it) {
				/* take next (may be beyond list) */
				++it;
				if (!*it) {
					/* went past the list */
					mylog(LOG_NOTICE, "no more playlists to choose");
					free(plsel);
					plsel = NULL;
					mymqttpub("playlist/selected", 0, NULL);
					send_mpd(mpdsock, "stop");
					return;
				}
			}
			if (!*it)
				/* take first again,
				 * either the last was not in the list
				 * or it was the last in the list
				 */
				it = pls;
			value = *it;
			free(pls);
			mymqttpub("playlist/selected", 0, value);
			plselmulti = 1;

		} else {
			if (!plsel && playing) {
				/* currently playing, but outside playlist/select */
				mylog(LOG_NOTICE, "stop playing");
				send_mpd(mpdsock, "stop");
				mymqttpub("playlist/selected", 0, NULL);
				return;
			}
			if (plsel) {
				free(plsel);
				plsel = NULL;
				send_mpd(mpdsock, "stop");
				if (plselmulti)
					mymqttpub("playlist/selected", 0, NULL);
				return;
			}
			plselmulti = 0;
			value = pls[0];
			free(pls);
		}
		mylog(LOG_NOTICE, "select playlist '%s' -> '%s'", plsel ?: "", value);
		if (plsel)
			free(plsel);
		plsel = strdup(value);
		plselt = now;
		goto playlist;

	} else if (!strcmp(subtopic, "playlist/set")) {
playlist:;
		char *mods;

		mods = strchr(value, ',');
		if (mods) {
			/* cut string */
			*mods++ = 0;
			/* create commands */
			mods = (char *)modifiers_to_cmds(mods);
		}
		if (!strncmp(value, "dir:", 4)) {
			send_mpd(mpdsock, "listall \"%s\"%s", value+4, mods ?: "");
			/* reset table */
			pltablefill = 0;
			pltablelisten = 1;
			pltableaction = TABLE_PLAY;
		} else
			send_mpd(mpdsock, "clear;load \"%s\"%s;play", value, mods ?: "");

	} else if (!strcmp(subtopic, "playlist/selected")) {
		/* ignore this, 'selected' is not a possible playlist */
	} else if (!strncmp(subtopic, "playlist/", 9)) {
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

static int connect_uri(const char *host, int default_port, int preferred_type)
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

	struct uri uri = {};

	lib_parse_uri(host, &uri);

	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = preferred_type,
		.ai_protocol = 0,
		.ai_flags = 0,
	}, *paddr = NULL, *ai;
	char portstr[32];

	sprintf(portstr, "%u", uri.port ?: default_port);
#ifdef AI_NUMERICSERV
	hints.ai_flags |= AI_NUMERICSERV;
#endif
	/* resolve host to IP */
	if (getaddrinfo(uri.host ?: "localhost", portstr, &hints, &paddr) < 0) {
		mylog(LOG_WARNING, "getaddrinfo %s %s: %s", uri.host, portstr, ESTR(errno));
		sock = -1;
		goto ready;
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
		mylog(LOG_WARNING, "connect %s:%u failed: %s", uri.host, uri.port ?: default_port, ESTR(errno));
	}
	freeaddrinfo(paddr);
ready:
	lib_clean_uri(&uri);
	return sock;
}

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

#define propcache(name)	propcache2(name, 1)
static char **propcache2(const char *propname, int create)
{
	int j;

	for (j = 0; j < nstate; j += 2) {
		if (!strcmp(propname, state[j]))
			return state+j+1;
	}
	if (!create)
		return NULL;

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

static const char *const songprops[] = {
	"Title",
	"Name",
	"Artist",
	"Author",
	"Album",
	"AlbumArtist",
	"Composer",
	"Track",
	"Genre",
	"Date",
};
static uint32_t props_seen;

static void prop_seen(const char *tok)
{
	int j;

	for (j = 0; j < sizeof(songprops)/sizeof(songprops[0]); ++j) {
		if (!strcasecmp(tok, songprops[j])) {
			props_seen |= 1 << j;
			return;
		}
	}
}

static void flush_unseen_props(void)
{
	int j;
	uint32_t mask;
	char **pvalue;

	for (j = 0, mask = 1; j < sizeof(songprops)/sizeof(songprops[0]); ++j, mask <<= 1) {
		if (props_seen & mask)
			continue;
		pvalue = propcache2(songprops[j], 0);
		if (pvalue && *pvalue && **pvalue) {
			/* clear property */
			free(*pvalue);
			*pvalue = NULL;
			mymqttpub(songprops[j], 1, "%s", "");
		}
	}
}

/* modified strtok_r that does not return the final part without delimiter
 * so to avoid cutting half-received lines
 * remain is guaranteed to be the remaining part
 */
static char *mystrtok_r(char *str, const char *delim, char **remain)
{
	char *next;

	if (!str)
		str = *remain;
	if (!str)
		return NULL;

	next = strpbrk(str, delim);
	if (!next) {
		*remain = str;
		return NULL;
	}
	*next++ = 0;
	if (!*next)
		next = NULL;
	*remain = next;
	return str;



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
		mqtt_uri = optarg;
		break;
	case 'p':
		mpd_uri = optarg;
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
	mpdsock = connect_uri(mpd_uri, mpd_default_port, SOCK_STREAM);
	if (mpdsock < 0)
		exit(1);

	/* MQTT start */
	struct uri mquri = {};
	lib_parse_uri(mqtt_uri, &mquri);

	if (mquri.user && !mquri.pass)
		/* lookup password from .netrc */
		lib_netrc(mquri.host ?: "localhost", (char **)&mquri.user, (char **)&mquri.pass);

	mosquitto_lib_init();
	sprintf(mqtt_name, "%s-%i", NAME, getpid());
	mosq = mosquitto_new(mqtt_name, true, 0);
	if (!mosq)
		mylog(LOG_ERR, "mosquitto_new failed: %s", ESTR(errno));
	/* mosquitto_will_set(mosq, "TOPIC", 0, NULL, mqtt_qos, 1); */

	mosquitto_log_callback_set(mosq, my_mqtt_log);
	mosquitto_message_callback_set(mosq, my_mqtt_msg);

	if (mquri.user || mquri.pass) {
		ret = mosquitto_username_pw_set(mosq, mquri.user, mquri.pass);
		if (ret)
			mylog(LOG_ERR, "mosquitto_username_pw_set(%s, %s): %s",
					mquri.user ?: "NULL", mquri.pass ? "***" : "NULL",
					mosquitto_strerror(ret));
	}

	ret = mosquitto_connect(mosq, mquri.host ?: "localhost",
			mquri.port ?: mqtt_default_port, mqtt_keepalive);
	if (ret)
		mylog(LOG_ERR, "mosquitto_connect %s:%i: %s", mquri.host ?: "localhost",
				mquri.port ?: mqtt_default_port, mosquitto_strerror(ret));

	lib_clean_uri(&mquri);

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
	recvbuf[0] = 0;

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
			char *tok, *saved, *value, **pcache;
			char valbuf[32];
			__attribute__((unused))
			char *outputname, outputid[32];
			int filled;

			filled = strlen(recvbuf);
			if (filled >= sizeof(recvbuf)-1)
				mylog(LOG_ERR, "recv mpd: buffer filled");
			filled = 0;
			/* read mpd changes */
			ret = recv(mpdsock, recvbuf+filled, sizeof(recvbuf)-1-filled, 0);
			if (ret < 0)
				mylog(LOG_ERR, "recv mpd: %s", ESTR(errno));
			if (ret == 0)
				mylog(LOG_ERR, "recv mpd: closed");
			recvbuf[filled+ret] = 0;
			/* TODO: clear absent state entries */
			for (tok = mystrtok_r(recvbuf, "\n\r", &saved); tok; tok = mystrtok_r(NULL, "\n\r", &saved)) {
				if (!strncmp(tok, "OK", 2) || !strncmp(tok, "ACK", 3)) {
					if (!strncmp(tok, "ACK", 3))
						mylog(LOG_WARNING, "%s", tok);
					/* command returned */
					mpdncmds -= 1;
					mylog(LOG_INFO, "< '%s'", tok);
					flush_unseen_props();
					continue;
				}
				value = propvalue(tok);
				if (!value)
					continue;

				if (!strcmp(tok, "directory"))
					continue;
				if (pltablefill && strcmp(tok, "file")) {
					/* playlist request, ended,
					 * and something else received */
					if (pltableaction == TABLE_CHOOSE1) {
						int idx;
						srand48(time(NULL));
						idx = drand48()*pltablefill;
						send_mpd(mpdsock, "clear;add %s;play", pltable[idx]);

					} else if (pltableaction == TABLE_PLAY) {
						int j;
						sendto_mpd_pre(mpdsock);
						sendto_mpd_direct(mpdsock, "clear\n");
						for (j = 0;j < pltablefill; ++j)
							sendto_mpd_direct(mpdsock, "add \"%s\"\n", pltable[j]);
						sendto_mpd_direct(mpdsock, "play\n");
						sendto_mpd_post(mpdsock);
					}
					pltablefill = 0;
					/* stop recording files */
					pltablelisten = 0;
				}
				/* replace 'state' */
				if (!strcmp(tok, "state")) {
					//mylog(LOG_INFO, "< '%s: %s'", tok, value);
					tok = "play";
					value = !strcmp(value, "play") ? "1" : "0";
					playing = *value == '1';
					playingt = time(NULL);
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
				} else if (!strcmp(tok, "file")) {
					/* mark all properties as unseen */
					props_seen = 0;
				}

				prop_seen(tok);
				pcache = propcache(tok);
				if (strcmp(value, *pcache ?: "")) {
					if (*pcache)
						free(*pcache);
					*pcache = strdup(value);
					/* publish */
					mymqttpub(tok, 1, value);
				}
			}
			if (saved)
				memmove(recvbuf, saved, strlen(saved)+1);

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
