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
2. FFmpeg

For Linux DVB server:
3. Linux DVB headers
4. Linux udev
5. POCO libraries


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
$ echo set <filename> | 9p write ommrenderer/ctl
$ echo play | 9p write ommrenderer/ctl
$ echo stop | 9p write ommrenderer/ctl
```

Start local server:
```
$ ommserve media.db &
```

```
Show content of server:
$ 9p ls ommserver
$ 9p ls ommserver/1
$ 9p read ommserver/1/meta
```

Play media from local server:
```
$ echo set ommserver/1/data | 9p write ommrenderer/ctl
$ echo play | 9p write ommrenderer/ctl
$ echo stop | 9p write ommrenderer/ctl
```

Show content of remote server running on 192.168.1.83, port 4567:
```
9p -a tcp!192.168.1.83!4567 ls
```

Play media from remote server:
```
$ echo set tcp!192.168.1.83!4567/1/data | 9p write ommrenderer/ctl
$ echo play | 9p write ommrenderer/ctl
$ echo stop | 9p write ommrenderer/ctl
```

## References

1. [Plan 9 from user space](https://9fans.github.io/plan9port)
2. [FFmpeg video player](https://github.com/rambodrahmani/ffmpeg-video-player)
