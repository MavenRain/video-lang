# video-lang

video-lang is a typed language for video edits. A program trims, scales,
crops and pads a video, and burns text into it. The compiler `langc` checks
the program and writes an mp4 file. It also prints the ffmpeg command that
makes the same edit. The decoded frames of the two outputs are equal
(ruling R1).

video-lang comes from lang-template with the tcc-media host. The host is C
code that TinyCC builds and that links the FFmpeg libraries and FreeType.
`design/DESIGN.md` is the domain design. `SPEC.md` is the specification.

## Quick start

Run these commands from the repository root. The outputs go into `build/`.

```sh
export PATH=/opt/homebrew/opt/ffmpeg-full/bin:$PATH
make
build/langc check test/media/text.lang
ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc2=size=320x240:rate=25:duration=2 -f lavfi -i sine=frequency=440:duration=2 -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest build/in.mp4
build/langc build test/media/text.lang hello build/in.mp4 /System/Library/Fonts/Supplemental/Arial.ttf -o build/ours.mp4
build/langc ffmpeg test/media/text.lang hello build/in.mp4 /System/Library/Fonts/Supplemental/Arial.ttf -o build/ref.mp4 > build/ref.sh
rm -f build/ref.mp4
sh build/ref.sh
ffmpeg -nostdin -loglevel error -y -i build/ours.mp4 -map 0 -f framemd5 build/ours.md5
ffmpeg -nostdin -loglevel error -y -i build/ref.mp4 -map 0 -f framemd5 build/ref.md5
cmp build/ours.md5 build/ref.md5 && echo equal
```

`make` builds `build/langc`. The `ffmpeg` line makes a 2 second test input
with video and audio. `langc build` writes `build/ours.mp4`. `langc ffmpeg`
writes the reference command to `build/ref.sh`, and `sh` runs it. The last
3 lines compare the decoded frames of the two files. The last line prints
`equal`.

To run the tests:

```sh
make check
make perf
```

`make check` runs the full gate. It takes several minutes. `make perf` runs
the 1080p checks. `docs/VALIDATION.md` gives the results of both.

## Verbs

| Command | Result |
|---|---|
| `langc check PROG` | Checks the program. Prints `ok`, or one refusal line. |
| `langc build PROG MAIN IN... -o OUT` | Applies the entry `MAIN` to the input files and writes the mp4 file `OUT` (libx264 and aac). |
| `langc ffmpeg PROG MAIN IN... -o OUT` | Prints the ffmpeg command that makes the same edit. It writes no media. |

Each parameter of `MAIN` is a `Video` or a `Font`. The input files follow
the order of the parameters: an mp4 file for a `Video`, a TrueType file for
a `Font`. Exit 0 is success. Exit 1 is a refused program. Exit 2 is a usage
or IO error. A refusal writes one line to stderr: `langc: CODE: NAME:
message`. `SPEC.md` section 1 also gives `langc eval` and the JSON form of
`langc build`.

## Example: burnText

This program is in `test/media/text.lang`:

```
def st : Font -> Style := fun (f : Font) => style f 32 16777215 10 20
def hello : Video -> Font -> Video := fun (v : Video) (f : Font) => burnText (st f) (cons (pair (interval 5 9) "Hi") nil) v
```

`style f 32 16777215 10 20` uses the font `f` at size 32, the color
16777215 (`0xffffff`, white) and the position x 10, y 20. `hello` draws the
text `Hi` on frames 5 to 9 of the video `v`. `langc ffmpeg` prints this
filter for it:

```
drawtext=fontfile=/System/Library/Fonts/Supplemental/Arial.ttf:text=Hi:expansion=none:fontsize=32:fontcolor=0xffffff:x=10:y=20:enable='between(n,5,9)'
```

## The R1 claim

For each program and input, the output of `langc build` and the output of
the `langc ffmpeg` command have equal decoded frames. The check is the
framemd5 of all the streams, video and audio, and the frame count. The
container bytes are not compared. `make check` checks R1 on 40 media
programs. `make perf` checks R1 on 6 runs at 1920x1080, up to 60 seconds.

## Files

- `SPEC.md`: the specification.
- `design/DESIGN.md`: the domain design. It is the only source of the core
  types and the core operations.
- `formers/FORMERS.md`: the type formers. `formers/tcc-media.md` gives their
  status on the tcc-media host.
- `probe/CAPABILITY.md`: the host probe.
- `docs/STATUS.md` and `docs/VALIDATION.md`: the status and the gate results.
- `docs/host/README.md`: the media host, its commands and its limits.
- `domain/`: the domain definitions. `domain/README.md` tells how to change
  them.
- `src/`: the compiler. `test/`: the gate and the C test programs.

## Requirements

- TinyCC (`TCC`, default `tcc`). The tested version is 0.9.28rc on AArch64
  Darwin.
- A C compiler `cc` for `make check-clang` (`CC`, default `cc`).
- Homebrew `ffmpeg-full` 9.0.2 (libavformat 63.1.102). This is the pin. The
  build links its libraries through `pkg-config`: libavfilter, libavformat,
  libavcodec, libswscale, libswresample, libavutil and freetype2. The
  reference command uses its `ffmpeg` command. Set `FFMPEG_PREFIX` to use a
  different installation (default `/opt/homebrew/opt/ffmpeg-full`). The
  Homebrew `ffmpeg` formula has no `drawtext` filter, thus it cannot make
  the burnText reference.
- The Arial font at `/System/Library/Fonts/Supplemental/Arial.ttf`. The gate
  and `make perf` use it.
- For `make check`: POSIX `sh`, `awk`, `cmp`, `rg` and Node.

## License

MIT OR Apache-2.0. See `LICENSE-MIT` and `LICENSE-APACHE`.
