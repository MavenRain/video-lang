# Validation

Date: 2026-10-12. TinyCC 0.9.28rc (AArch64 Darwin), Apple clang 21.0.0,
Homebrew `ffmpeg-full` 9.0.2 (libavformat 63.1.102), Node 23.10.0. Host
executable: `build/langc`. The source files are the same as at commit
aae4c44.

## The gate

`make check FFMPEG_PREFIX=/opt/homebrew/opt/ffmpeg-full` passes with zero
failures. It builds `build/langc` and the C test programs with TinyCC, and
`make check-clang` gives no warnings. It runs 434 cases:

| Group | Cases | Source |
|---|---|---|
| examples | 12 | `test/gate.sh` |
| parse refusals | 17 | `test/gate.sh`, `test/parse` |
| check refusals | 34 | `test/gate.sh`, `test/check` |
| evaluations | 35 | `test/gate.sh` |
| JSON goldens | 12 | `test/gate.sh`, `test/json` |
| build refusals | 2 | `test/gate.sh`, `test/emit` |
| media R1 | 60 | `test/gate.sh`, `test/media`, `test/ffmpeg` |
| media laws | 29 | `test/gate.sh`, `test/media/laws.txt` |
| media refusals | 26 | `test/gate.sh`, `test/media/refuse.txt` |
| regressions | 6 | `test/regressions.js` |
| printer | 2 | `test/printer-regressions.js` |
| media regressions | 52 | `test/mediatool.c` |
| media IO | 140 | `test/mediareview.c` |
| perf comparator | 7 | `test/mediaperfcheck.c` |

A media R1 case runs `langc build` and the `langc ffmpeg` command on the
same program and inputs. Then it compares the framemd5 of all the streams
and the frame count. The gate makes its inputs at run time with ffmpeg
(`testsrc2` 320x240 at 25 fps, with and without a 440 Hz `sine` track, and
variable frame rate inputs with dropped frames, a rate change with audio,
off-grid pts, and a collision after rounding). It also makes short GOP
inputs (libx264 `-g 12 -bf 3`, 25 fps, 4 s; mp4, Matroska, VFR and open
GOP) for cuts before, at and after a key frame, a cut to the last frame, a
cut of one frame, nested cuts and text across a key frame. For M6 it makes
av160.mp4 (160x120) and av22k.mp4 (22050 Hz audio) for the `CONCAT_SHAPE`
rows; the concat rows join two or three inputs, the same input twice and
VFR with CFR, and push trim, scale, crop, pad and text through the seam.
The C media IO cases also compare VFR outputs with continuous and gapped audio in mp4 and
Matroska sources, including whole, trim, nested trim and scale. They check
the decode window on known key layouts, the timestamp refusal, the skip of
the packets before and after the window (a corrupt packet there does not
change the output), and a rawvideo decode without decoder delay. The
concat cases check pushed trims and text, nested concat ranks, the shape
refusal on SAR, a 24 fps second input, a faster second input
(`MEDIA_TIMESTAMPS`), and short and long audio at the seam. The segment
cases check that a pushed scale and a pushed text entry land in each
segment and that the video pts continue at the seam.

## Performance at 1920x1080

`make perf FFMPEG_PREFIX=/opt/homebrew/opt/ffmpeg-full` passes. The make
rules make the inputs with ffmpeg: `testsrc2` 1920x1080 at 25 fps and a
440 Hz `sine` track, 10, 20 and 60 seconds (s10 7181977, s20 14470717, s60
43413000 bytes). OURS is `langc build`. REF is the `langc ffmpeg` command.
RSS is the peak resident size in bytes (`ru_maxrss`), and the times are in
seconds. The six `whole` and `chain` rows are from commit fedc686. The
`deep` row (a cut of 100 frames at the end of s60, positions 1400 to 1499)
is from commit f28d402, in a run under machine load; with the decode
window (M5) the build decodes one GOP, not the 1500 frames of s60. The
`join` row (s10 followed by s10 through `concat`, M6) is from commit
aae4c44.

| Input | Entry | Frames | OURS RSS | REF RSS | OURS s | REF s | R1 |
|---|---|---|---|---|---|---|---|
| s10 | whole | 250 | 854228992 | 879755264 | 4.41 | 3.89 | equal |
| s10 | chain | 225 | 871448576 | 892960768 | 3.27 | 3.45 | equal |
| s20 | whole | 500 | 885735424 | 878968832 | 5.77 | 6.45 | equal |
| s20 | chain | 475 | 881098752 | 897417216 | 4.21 | 4.41 | equal |
| s60 | whole | 1500 | 904462336 | 882671616 | 18.26 | 17.91 | equal |
| s60 | chain | 1475 | 906215424 | 895631360 | 15.90 | 15.64 | equal |
| s60 | deep | 100 | 905510912 | 871038976 | 1.53 | 3.45 | equal |
| s10 | join | 500 | 931610624 | 933085184 | 2.58 | 3.33 | equal |

The cap is 4000000000 bytes. A run fails at the cap or above it. Memory
grows with the compressed input size, not with the decoded frames (O-3):
from s10 to s60 the peak grew 50233344 bytes (whole) and 34766848 bytes
(chain), and the input grew 36231023 bytes. At commit 5cfc171,
`/usr/bin/time -l` on the s10 `chain` run gave a maximum resident set size
of 887357440 bytes, and the runner gave 891994112 bytes for the same run.

## Decode window (M5)

The video decode of a cut starts at the last decode key at or before the
first kept position that splits presentation order, or at packet 0 if none
exists (`SPEC.md` O-3). It sends no more video packets after the decoder
gives the last kept frame. Frames from the selected start through that
position are checked against the index; buffered frames emitted afterward
are discarded without a pts check (`src/media.c:1401-1439`). At commit
f28d402, four mutants of `src/media.c` fail the media IO regressions
(`make -B` for each rebuild): the key after the first kept position
(17 failures), the position seed plus one, a key test with `<` in place of
`<=`, and each key packet as a decode key (8 failures).

The fifth mutant, a stop at the position before the last kept frame,
survived the 120 media IO cases and the 50 R1 rows at f28d402 on the
libx264 fixtures: the decoder runs with `threads=auto`, and frame threading
holds the packet of the last kept position before it gives the frame
before it, so the output was the same. A rawvideo source has no decoder
delay. Commit b86fefe adds the case "a decoder without delay gives position
hi (rawvideo {13, 30})", which fails under the mutant (`MEDIA_DECODE`) and
passes with the original encode loop.

Real world, ffmpeg-full 9.0.2, a 1920x1080 H.264 source of 1290 s (38700
frames, 337 key packets, GOP about 220): a cut of 394 frames (positions 309
to 702) built in 4.93 s (first run, peak 1039 MB) and 4.16 s (warm, peak
2160 MB), equal to the reference command by framemd5 (1025 lines). At
commit 98d3f06 the same build took 46.58 s and 29.99 s. A cut of 360
frames in a second source built in 2.14 s (peak 880 MB), equal to the
reference.

## Concatenation (M6)

At commit aae4c44, five mutants of `src/media.c` fail the gate (`make -B`
for each rebuild; each restore passes with zero failures):

| Mutant | Change | Rows that fail |
|---|---|---|
| m1 | the seam from the video end only (`seam`: the audio end ignored) | media IO `concat follows audio end past video seam` |
| m2 | the pushed segment length off by one (`cut_of`: `phi - plo` for `phi - plo + 1`) | R1 cseam and cburn; the law `seam = split`; 4 media IO cases |
| m3 | the `CONCAT_SHAPE` check dropped (`shape_check`) | the 6 `CONCAT_SHAPE` refusal rows (size, audio presence, sample rate, under `build` and `ffmpeg`); media IO `concat refuses different operand SAR` |
| m4 | a pushed step applied to segment 0 only (`cut_of`) | R1 cseam, cscale, ccrop, cpad and cburn; the laws scale, crop and pad distribute, and `seam = split`; the `EMPTY_VIDEO` refusal rows; 5 media IO cases |
| m5 | the audio clock reset at the seam (`audio_keep`) | all 10 R1 concat rows; the 5 concat laws; media IO `concat pads short audio to video seam` and `concat follows audio end past video seam` |

The gate fixtures end audio and video at the same instant, so only the
media IO case sees the longer audio tail under m1. Under m5 the muxer sees
the audio dts go back at the seam, so every concat R1 row fails.

Real world, ffmpeg-full 9.0.2: the two cuts of the decode window section
(394 frames from the 1290 s recording, 360 frames from the second source)
joined through `concat`. `langc build` took 5.2 s (peak 907 MB), and the
`langc ffmpeg` command 8.0 s (peak 930 MB). The outputs are equal by
framemd5 (1948 lines, 754 video frames).
