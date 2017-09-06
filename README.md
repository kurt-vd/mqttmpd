# mqttmpd

Control your MPD via MQTT

## How

This projects makes a bridge between MQTT & MPD.

# MQTT topic layout

I didn't want to put much logic in the bridge, and maintained names
from the MPD raw protocol, unless otherwise indicated.

## read/write topics

* mpd/play		0=paused, 1=play
* mpd/volume		volume: 0..1, 0.01 steps
* mpd/consume
* mpd/random
* mpd/repeat
* mpd/single
* mpd/playlist		read: playlist number (refer to MPD)
			write: playlist name to load.

Writing to these topics is done to xxx/set, e.g. mpd/play/set
Whenever applicable, MPD will change its property, and that
is returned to the topic as retained message.

## writeonly topics

* mpd/playlist/NAME	1: load playlist
			0: stop playing

This logic is necessary to assign home automation controls ...

## readonly topics

All properties in the MPD raw protocol are exported:

* mpd/Title
* mpd/Artist
* mpd/Album
* mpd/song
* mpd/songid
* mpd/time
* mpd/elapsed
* mpd/bitrate
* mpd/duration
* ...

## data normalization

Wherever possible, mqttautomation will use **0** and **1** for boolean variable,
ranges go from **0** to **1** (floating point).

This 0/1 normalization enables you to connect unrelated logic.
I use this to control my MPD using KNX wall switches, which emit 0 or 1.
