# OMM - Open Multimedia

- [Introduction](https://github.com/captaingroove/omm-9p#introduction)
- [Dependencies](https://github.com/captaingroove/omm-9p#dependencies)
- [Building from Source](https://github.com/captaingroove/omm-9p#building-from-source)
- [Usage](https://github.com/captaingroove/omm-9p#usage)
- [References](https://github.com/captaingroove/omm-9p#references)

## Introduction

OMM is a set of applications for playing multimedia streams in a distributed environment.

## Dependencies

1. Plan 9 from user space
2. FFmpeg

## Building from Source

Run `make`. Currently, no dependencies are checked.

## Usage

Play file from disk:
```bash
$ ommrender <filename>
```

Start server:
```bash
$ ommserve media.db &
```

```bash
Show contents of server:
$ 9p ls ommserver
$ 9p ls ommserver/1
$ 9p read ommserver/1/meta
```

Start renderer:
```bash
$ ommrender &
```

Play file from disk through server:
```bash
$ echo set <filename> | 9p write ommrenderer/ctl
$ echo play | 9p write ommrenderer/ctl
$ echo stop | 9p write ommrenderer/ctl
```

Play file from server:
```bash
$ echo set ommserver/1/data | 9p write ommrenderer/ctl
$ echo play | 9p write ommrenderer/ctl
$ echo stop | 9p write ommrenderer/ctl
```

## References

1. [Plan 9 from user space](https://9fans.github.io/plan9port)
2. [FFmpeg video player](https://github.com/rambodrahmani/ffmpeg-video-player)
