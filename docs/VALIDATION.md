# Validation

Date: 2026-10-10. TinyCC 0.9.28rc (AArch64 Darwin), Apple clang 21.0.0,
Homebrew `ffmpeg-full` 9.0.2 (libavformat 63.1.102), Node 23.10.0. Host
executable: `build/langc`. The source files are the same as at commit
5cfc171.

## The gate

`make check FFMPEG_PREFIX=/opt/homebrew/opt/ffmpeg-full` passes with zero
failures. It builds `build/langc` and the C test programs with TinyCC, and
`make check-clang` gives no warnings. It runs 289 cases:

| Group | Cases | Source |
|---|---|---|
| examples | 11 | `test/gate.sh` |
| parse refusals | 17 | `test/gate.sh`, `test/parse` |
| check refusals | 31 | `test/gate.sh`, `test/check` |
| evaluations | 29 | `test/gate.sh` |
| JSON goldens | 11 | `test/gate.sh`, `test/json` |
| build refusals | 2 | `test/gate.sh`, `test/emit` |
| media R1 | 25 | `test/gate.sh`, `test/media`, `test/ffmpeg` |
| media laws | 16 | `test/gate.sh`, `test/media/laws.txt` |
| media refusals | 21 | `test/gate.sh`, `test/media/refuse.txt` |
| regressions | 6 | `test/regressions.js` |
| printer | 2 | `test/printer-regressions.js` |
| media regressions | 52 | `test/mediatool.c` |
| media IO | 59 | `test/mediareview.c` |
| perf comparator | 7 | `test/mediaperfcheck.c` |

A media R1 case runs `langc build` and the `langc ffmpeg` command on the
same program and inputs. Then it compares the framemd5 of all the streams
and the frame count. The gate makes its inputs at run time with ffmpeg
(`testsrc2` 320x240 at 25 fps, with and without a 440 Hz `sine` track, and
one variable frame rate input).

## Performance at 1920x1080

`make perf FFMPEG_PREFIX=/opt/homebrew/opt/ffmpeg-full` passes. The make
rules make the inputs with ffmpeg: `testsrc2` 1920x1080 at 25 fps and a
440 Hz `sine` track, 10, 20 and 60 seconds (s10 7181977, s20 14470717, s60
43413000 bytes). OURS is `langc build`. REF is the `langc ffmpeg` command.
RSS is the peak resident size in bytes (`ru_maxrss`), and the times are in
seconds.

| Input | Entry | Frames | OURS RSS | REF RSS | OURS s | REF s | R1 |
|---|---|---|---|---|---|---|---|
| s10 | whole | 250 | 873152512 | 879591424 | 1.27 | 1.47 | equal |
| s10 | chain | 225 | 891994112 | 893452288 | 1.04 | 0.98 | equal |
| s20 | whole | 500 | 884998144 | 884015104 | 2.55 | 2.37 | equal |
| s20 | chain | 475 | 903659520 | 897384448 | 1.81 | 1.83 | equal |
| s60 | whole | 1500 | 922451968 | 886702080 | 6.50 | 6.61 | equal |
| s60 | chain | 1475 | 936312832 | 901152768 | 5.46 | 5.28 | equal |

The cap is 4000000000 bytes. A run fails at the cap or above it. Memory
grows with the compressed input size, not with the decoded frames (O-3):
from s10 to s60 the peak grew 49299456 bytes (whole) and 44318720 bytes
(chain), and the input grew 36231023 bytes. `/usr/bin/time -l` on the s10
`chain` run gave a maximum resident set size of 887357440 bytes. The runner
gave 891994112 bytes for the same run.
