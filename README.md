# OMM - Open Multimedia

- [Introduction](https://github.com/captaingroove/omm-9p#introduction)
- [Dependencies](https://github.com/captaingroove/omm-9p#dependencies)
- [Building from Source](https://github.com/captaingroove/omm-9p#building-from-source)
- [Usage](https://github.com/captaingroove/omm-9p#usage)
- [References](https://github.com/captaingroove/omm-9p#references)

## Introduction

OMM is a set of applications for playing multimedia streams in a distributed environment.

## Dependencies

For basic renderer and server:

1. P9light or Plan 9 from user space
2. libixp
3. LibVLC
4. SDL2
5. SQLite

Optionally, for the ffmpeg renderer backend:

1. FFmpeg libraries: avutil, avformat, avcodec, swscale, swresample

For the Linux DVB server:

1. Linux DVB headers
2. Linux udev
3. POCO libraries: Foundation, Util, XML, Zip

## Building from Source

Run `make`. Currently, no dependencies are checked.

## Usage

Play file from disk:
```
$ ommrender <filename>
```

Start renderer:
```
$ export VLC_PLUGIN_PATH=<path to OMM source directory>/build
# export DBUS_FATAL_WARNINGS=0
$ ommrender &
```

Control renderer from command line:
```
$ echo set file:///<absolute file path> | 9p write ommrender/ctl
$ echo play | 9p write ommrender/ctl
$ echo stop | 9p write ommrender/ctl
```

Start local server, where media.db is an SQLite database containing the meta data of the media objects:
```
$ ommserve media.db &
```

Serving DVB streams currently needs an XML file from a transponder scan:
```
$ ommserve media.db dvb.xml &
```

Show content of server:
```
$ 9p ls ommserve
$ 9p ls ommserve/1
$ 9p read ommserve/1/meta
```

Play media from local server (currently defunct):
```
$ echo set ommserve/1/data | 9p write ommrender/ctl
$ echo play | 9p write ommrender/ctl
$ echo stop | 9p write ommrender/ctl
```

Stream media from local server to other media player, e.g. mpv:
```
$ 9p read ommserve/1/data | mpv -
```

Show content of remote server running on 192.168.1.83, port 4567:
```
$ 9p -a tcp!192.168.1.83!4567 ls
```

Play media from remote server:
```
$ echo set 9p://tcp!192.168.1.83!4567/1/data | 9p write ommrender/ctl
$ echo play | 9p write ommrender/ctl
$ echo stop | 9p write ommrender/ctl
```

Quit renderer:
```
$ echo quit | 9p write ommrender/ctl
```

## References

1. [Plan 9 from user space](https://9fans.github.io/plan9port)
2. [FFmpeg video player](https://github.com/rambodrahmani/ffmpeg-video-player)
