# Validation

Date: 2026-10-11. TinyCC 0.9.28rc (AArch64 Darwin), Apple clang 21.0.0,
Homebrew `ffmpeg-full` 9.0.2 (libavformat 63.1.102), Node 23.10.0. Host
executable: `build/langc`. The source files are the same as at commit
7246d40.

## The gate

`make check FFMPEG_PREFIX=/opt/homebrew/opt/ffmpeg-full` passes with zero
failures. It builds `build/langc` and the C test programs with TinyCC, and
`make check-clang` gives no warnings. It runs 339 cases:

| Group | Cases | Source |
|---|---|---|
| examples | 11 | `test/gate.sh` |
| parse refusals | 17 | `test/gate.sh`, `test/parse` |
| check refusals | 31 | `test/gate.sh`, `test/check` |
| evaluations | 29 | `test/gate.sh` |
| JSON goldens | 11 | `test/gate.sh`, `test/json` |
| build refusals | 2 | `test/gate.sh`, `test/emit` |
| media R1 | 40 | `test/gate.sh`, `test/media`, `test/ffmpeg` |
| media laws | 24 | `test/gate.sh`, `test/media/laws.txt` |
| media refusals | 21 | `test/gate.sh`, `test/media/refuse.txt` |
| regressions | 6 | `test/regressions.js` |
| printer | 2 | `test/printer-regressions.js` |
| media regressions | 52 | `test/mediatool.c` |
| media IO | 86 | `test/mediareview.c` |
| perf comparator | 7 | `test/mediaperfcheck.c` |

A media R1 case runs `langc build` and the `langc ffmpeg` command on the
same program and inputs. Then it compares the framemd5 of all the streams
and the frame count. The gate makes its inputs at run time with ffmpeg
(`testsrc2` 320x240 at 25 fps, with and without a 440 Hz `sine` track, and
variable frame rate inputs with dropped frames, a rate change with audio,
off-grid pts, and a collision after rounding). The C media IO cases also
compare VFR outputs with continuous and gapped audio in mp4 and Matroska
sources, including whole, trim, nested trim and scale.

## Performance at 1920x1080

`make perf FFMPEG_PREFIX=/opt/homebrew/opt/ffmpeg-full` passes. The make
rules make the inputs with ffmpeg: `testsrc2` 1920x1080 at 25 fps and a
440 Hz `sine` track, 10, 20 and 60 seconds (s10 7181977, s20 14470717, s60
43413000 bytes). OURS is `langc build`. REF is the `langc ffmpeg` command.
RSS is the peak resident size in bytes (`ru_maxrss`), and the times are in
seconds. The table is from commit fedc686: commit 7246d40 changes the
audio pts only, not the encode loop.

| Input | Entry | Frames | OURS RSS | REF RSS | OURS s | REF s | R1 |
|---|---|---|---|---|---|---|---|
| s10 | whole | 250 | 854228992 | 879755264 | 4.41 | 3.89 | equal |
| s10 | chain | 225 | 871448576 | 892960768 | 3.27 | 3.45 | equal |
| s20 | whole | 500 | 885735424 | 878968832 | 5.77 | 6.45 | equal |
| s20 | chain | 475 | 881098752 | 897417216 | 4.21 | 4.41 | equal |
| s60 | whole | 1500 | 904462336 | 882671616 | 18.26 | 17.91 | equal |
| s60 | chain | 1475 | 906215424 | 895631360 | 15.90 | 15.64 | equal |

The cap is 4000000000 bytes. A run fails at the cap or above it. Memory
grows with the compressed input size, not with the decoded frames (O-3):
from s10 to s60 the peak grew 50233344 bytes (whole) and 34766848 bytes
(chain), and the input grew 36231023 bytes. At commit 5cfc171,
`/usr/bin/time -l` on the s10 `chain` run gave a maximum resident set size
of 887357440 bytes, and the runner gave 891994112 bytes for the same run.
