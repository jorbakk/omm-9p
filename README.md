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

1. Plan 9 from user space
2. FFmpeg libraries: avutil, avformat, avcodec, swscale, swresample
3. SDL2
4. SQLite

For Linux DVB server:

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
$ ommrender &
```

Control renderer from command line:
```
$ echo set <filename> | 9p write ommrender/ctl
$ echo play | 9p write ommrender/ctl
$ echo stop | 9p write ommrender/ctl
```

Start local server, where media.db is an SQLite database containing the meta data of the media objects:
```
$ ommserve media.db &
```

```
Show content of server:
$ 9p ls ommserve
$ 9p ls ommserve/1
$ 9p read ommserve/1/meta
```

Play media from local server:
```
$ echo set ommserve/1/data | 9p write ommrender/ctl
$ echo play | 9p write ommrender/ctl
$ echo stop | 9p write ommrender/ctl
```

Show content of remote server running on 192.168.1.83, port 4567:
```
9p -a tcp!192.168.1.83!4567 ls
```

Play media from remote server:
```
$ echo set tcp!192.168.1.83!4567/1/data | 9p write ommrender/ctl
$ echo play | 9p write ommrender/ctl
$ echo stop | 9p write ommrender/ctl
```

## References

1. [Plan 9 from user space](https://9fans.github.io/plan9port)
2. [FFmpeg video player](https://github.com/rambodrahmani/ffmpeg-video-player)
