# Host capability probe: tcc-media

Date: 2026-10-10. Host tree: this repository at commit 2c6e29b, read only.
Driver: `build/langc` (TinyCC 0.9.28rc, built by `make` on 2026-10-10).
FFmpeg pin: Homebrew ffmpeg-full 9.0.2 (libavformat 63.1.102). Other work
ran on the machine at the same time, so each wall time is an upper bound.

To run the probe again: `make check FFMPEG_PREFIX=/opt/homebrew/opt/ffmpeg-full`
runs the gate rows, and `make perf FFMPEG_PREFIX=/opt/homebrew/opt/ffmpeg-full`
runs the 1080p cost rows. `make perf` rejects a measured build peak of
4000000000 bytes or more (`test/mediaperf.c:155`).
`probe/guard.py --rss-mb N --timeout S -- COMMAND` stops a command above
N MB resident memory or above S seconds. The gate writes only below a new
temporary directory. `make perf` writes its clips to `test/media/perf/`,
which git ignores. The largest process in the probe used 936312832 bytes.

## Questions

Each host must answer these questions. Each answer is one numbered section
below. An answer sets the status of a former in `formers/tcc-media.md`.

| Question | Section | Formers |
|---|---|---|
| Which commands check a file, print a normal form, build and run? | 1 | all |
| Does the host take several files, or must the driver join them? | 2 | all |
| Which recursion does the host accept, and how does it refuse the rest? | 3 | F6, F7 |
| Which `Nat` primitives exist? Does `Nat` have an eliminator? | 4 | F6, F7 |
| How does a result leave the host? | 5 | target |
| What do check and build cost (time and resident memory)? | 6 | all |
| Can a family take a parameter? Can a constructor of such a family appear in a term? | 7 | F4, F12, F15 |
| Can a program project a Sigma? Does the kernel accept Sigma eta? | 8 | F11 |
| How does a program use equality: transport, cong, an index type? | 9 | F12, F13 |
| How does the host report an axiom? | 10 | R2 |

## Probes

The probe programs ran outside this repository. P1 to P4 ran first on
FFmpeg 8.1.2, then again on the pin. The sections below cite the results on
the pin.

| Id | Result on the pin |
|---|---|
| P1 | TinyCC compiles and links the libav* libraries with plain `pkg-config --cflags --libs` flags. A decode of 50 frames gives each PTS in increasing order. |
| P2 | Trim 10..29, then `sws_scale` with SWS_BICUBIC: 20 of 20 frames equal `ffmpeg -vf trim=start_frame=10:end_frame=30,scale=160:120`. |
| P3 | `scale=240:180,scale=160:120` against `scale=160:120`: 0 of 50 frames are equal. |
| P4 | Two runs of the same ffmpeg encode (libx264, aac, .mp4) give the same bytes. |
| P5 | The stock Homebrew ffmpeg has no `drawtext` filter. |
| P6 | TinyCC links libavfilter and libswresample. A graph `crop=160:120:10:20` in process: 50 of 50 frames equal the ffmpeg `crop`. |
| P7 | Audio trim: 35 of 35 audio frames equal ffmpeg `atrim=start_sample=S:end_sample=E`. S and E count decoded audio values after the AAC priming skip. |
| P8 | ffmpeg `drawtext` is deterministic: two runs give the same framemd5 and the same file bytes. |
| P9 | A graph chosen in process by the frame position equals ffmpeg `enable='between(n,5,9)'` on 50 of 50 frames. `between` includes both ends. |
| P10 | After a trim, the `n` of `drawtext` counts the frames into `drawtext`, from 0. |
| P11 | In ffmpeg, `drawtext` then `trim` equals `trim` then `drawtext` with `n` moved by the trim start. |
| P12 | `scale`, then `drawtext`, then `pad` in one graph: 50 of 50 frames equal ffmpeg. |
| P13 | Text at an odd x and y on yuv420p: 50 of 50 frames equal ffmpeg, and the frames differ from x and y less 1. |
| P14 | Two text entries on the same frames draw in list order. |
| P15 | A space and the UTF-8 text `café €5` give equal frames in process and in ffmpeg. |
| P16 | The escaped `text=` value gives the same frames as `textfile=` with the raw bytes. |
| P17 | VFR source (ffmpeg-full 9.0.2; vfr2 = 25 fps, then 12.5 fps from position 30; stream tb 1/12800). `-fps_mode passthrough` gives the same framemd5 as no flag on vfr2 (trim 20:40, whole, scale) and on the CFR cases whole, small, vonly, tail and pick: the plain mp4 output does not duplicate or drop a frame. The output video tb is 1/fr (fr = r_frame_rate), not the stream tb: `#tb 1/25` for vfr2 and vfr3 (stream tb 1/90000), `#tb 6/149` for vfr5. The mp4 timescale is the rate numerator doubled until it is 10000 or more (12800, 19072). Each kept frame has pts = its source pts minus the pts of the first kept frame, in ticks of 1/fr: trim 20:40 gives 0 1 2 ... 9 11 13 ... 29. The last frame keeps its own packet duration (trim 20:31 ends on the first 12.5 fps frame with duration 1; trims that end at position 40 and the whole video end with 2). Other durations are the dts steps of the output. |
| P17b | Rounding to 1/fr is to the nearest tick, not down: on vfr5 (the 12.5 fps part 13 ms late, r 149/6) position 21 is 0.993 tick after position 20 and gets pts 1. fftools adds 1/2^17 to a value that is not an integer, so a tie goes up. Two frames that round to one pts (vfrc: positions 29 and 30 are 4 ms apart at fr 25): the CLI exits 0, libx264 prints "non-strictly-monotonic PTS" and the muxer moves a dts ("This may result in incorrect timestamps"). We refuse that cut with `MEDIA_TIMESTAMPS`. |
| P-M5-1 | Key layout (ffprobe): libx264 `-g 12 -bf 3` fixtures have a key packet at the video index 0, 12, 24, ...; the longest run of packets with a pts below the running maximum is 3; each GOP is closed. A 1290 s 1080p H.264 recording has 337 key packets, GOP about 220, each closed. |
| P-M5-2 | Exactness: a decode from each key packet gives the frames of the decode from packet 0 at the same positions, with the first frame at the pts of the key. 54 of 54 key decodes on 20 fixtures, 8 of 8 on the recording. |
| P-M5-3 | In a decode from packet 0, the pts of each frame equals the indexed pts of its position, on the 20 fixtures and on the recording (38700 of 38700). |
| P-M5-4 | An audio only decode of the recording takes 0.6 s; the video decode from packet 0 takes 33 s. Thus the audio decode stays whole (D-M5-3). |
| P-M5-5 | Baseline at commit 98d3f06: a cut of 394 frames deep in the recording built in 46.58 s (first run) and 29.99 s (warm), peak 1023 MB and 1917 MB. With the decode window: 4.93 s and 4.16 s, byte equal outputs. |
| P-M6-1 | Seam timing (ffmpeg-full 9.0.2 `concat` filter, `-fps_mode passthrough`): the seam after the first segment is the maximum of its audio end (last pts plus the sample duration) and video mean rate end (last pts times n/(n - 1), or 0 for one frame) in 1/1000000. Later seams accumulate these lengths on that clock. Video pts round to the 1/fr grid of the first segment; audio pts round to 1/sample_rate, and silence fills short audio up to the unrounded seam. av + av: seam 0.8 s exact, 70560 audio samples. vfr2 + av: graph seam 2.977255 s, file frame 52 at 2.96 s, 225 silence samples. Audio longer than the video: seam 3.0 s, a 1.04 s hold on the last frame. The file video tb is 1/12800 and r_frame_rate 25/1 in every case. |
| P-M6-2 | Shape: ffmpeg refuses a size mismatch (`Input link in0:v0 parameters (size 160x120, SAR 1:1) do not match`, exit 234). It also refuses different known sample aspect ratios: two 320x240 inputs with SAR 1:1 and 2:1 fail with exit 234. An unknown SAR does not itself cause `CONCAT_SHAPE`. A missing audio stream is a graph binding error (exit 234). A 22050 Hz input and a stereo input are converted (exit 0, 44100 Hz mono). `CONCAT_SHAPE` is stricter: it also refuses a sample rate or a channel layout difference. |
| P-M6-3 | Laws on pixels: the two sides are equal by framemd5 and by bytes for associativity (65 frames), scale distribution (40 frames) and the trim split at the seam (25 frames). |
| P-M6-4 | One input twice: two `-i` of one file and one `-i` with `split` give equal outputs by framemd5 and by bytes (40 frames); peak RSS 68.5 MB against 57.5 MB and 64.0 MB. The plan repeats the input (D-M6-5). |
| P-M6-5 | Front end sites: the Op table row `{"concat", 2, RULE_MEDIA, OP_CONCAT, OP_VIDEO, 0}`; a stuck `concat` keeps both operands; `src/json.c` never prints a media op. Before M6, `concat` refused as `NAME_UNKNOWN`. |
| P-M6-6 | Cost: the packet bytes of excerptA (1118069975 video, 30964857 audio) plus excerptB (9972460, 718840) are about 1.16 GB, under the cap of 4 GB (D-M6-7). A two input reference run (clip3 then clip12) took 9.3 s at a peak of 912 MB; the seam frame 394 is at pts 13.133333 s = 394/30. |
| B1 | `expansion=normal` with `%{n}`: the frame number is relative, so trim and text do not commute. |
| B2 | `expansion=normal` with `100%`: ffmpeg stops with "Stray %", exit 234. |
| B3 | `fontfile=` names a missing file: ffmpeg exits 0 and uses fontconfig. |
| B4 | No `fontfile`: the frames depend on the fonts of the machine. |
| B5 | `text=` empty: the frames equal the frames without `drawtext`. |
| B6 | `fontsize=0`: ffmpeg draws pixels. |
| B7 | `fontsize` not set: ffmpeg uses 16. |
| B8 | `fix_bounds=1` moves text that the frame edge clips (50 frames change). |
| B9 | Text fully outside the frame: the frames equal the frames without `drawtext`. |

## 1. Commands

| Job | Command | Result |
| --- | --- | --- |
| Check | `build/langc check PROG` | Exit 0, or exit 1 and a refusal code (gate rows: parse 17, check 31) |
| Print normal form | `build/langc eval PROG NAME [ARGS...]` | The value, or `trap` (gate row: evals 29) |
| Build | `build/langc build PROG MAIN IN... -o OUT` | A media file that the host makes in process with libav* (gate row: media R1 50) |
| Run | `build/langc ffmpeg PROG MAIN IN... -o OUT` | The ffmpeg command for the same program. The gate runs it as the reference for R1. |

- `build/langc build PROG [-o OUT]` writes the JSON document (gate row:
  json 11).
- Exit codes: 0 ok, 1 refused, 2 usage or IO (`src/main.c:2`). An `eval`
  argument refusal (EVAL_ARGS) exits 2 (`test/gate.sh:101`).
- Each command takes one program file.
- Not probed: an FFmpeg other than the pin.

## 2. Several files

The host takes one program file. The grammar has no import form
(`src/front/parser.c:1`). `make` joins `domain/domain.lang` and
`domain/video.lang` into `build/domain-all.lang` (`Makefile:34`). Then
`tcc -run gen/embed.c` puts that file into `build/domain.c`. The host loads
the domain before the program (`src/front/front.c:13`). Gate row:
examples 11.

## 3. Recursion and totality

The host accepts no general recursion. A program repeats work only with
`fold` (over `Nat`, `List` or a domain family) and with `unfold`, which has
a step limit (`src/front/check.c:115`). The checker refuses `def rec` with
"def rec is refused (rule R3); use fold or unfold" (`REFUSE_REC`,
`src/front/check.c:1523`). It refuses a use of the same or a later
definition with `REFUSE_REC` (`src/front/check.c:1128`). Gate rows:
`rec.lang`, `self.lang` and `later.lang` in `test/check/expect.txt`. A
media program needs no recursion: it is at most 64 steps (`STEP_MAX`,
`src/media.c:40`, refusal `MEDIA_LIMIT`).

## 4. Nat

The primitives are `natAdd`, `natSub`, `natMul`, `natEq` and `natLe`
(`src/front/check.c:120`). `natAdd` and `natMul` trap on overflow.
`natSub` stops at 0. The largest literal is 18446744073709551615, and a
larger literal gives `LEX_NAT_RANGE` (`src/front/lexer.c:138`). The
eliminator is `fold` over `Nat`. A frame position is a `Nat`. A literal
`interval lo hi` with lo > hi gives `INTERVAL_RANGE`. The media build
refuses an interval with lo > hi that reaches it (`INTERVAL_ORDER`,
`src/media.c:330`). Gate rows: the two `trap` rows in
`test/eval/expect.txt`.

## 5. How a result leaves the host

`langc build PROG MAIN IN... -o OUT` decodes the inputs, runs the steps in
process (libavfilter graphs and `sws_scale`) and encodes OUT with the
default encoders of the container (`src/media.c:1129`; libx264 and aac for
.mp4). It writes a temporary file and renames it to OUT only when the build
succeeds (`src/media.c:1701`). R1 compares the decoded frames of OUT with
the decoded frames of the `langc ffmpeg` output: framemd5 over all streams
(`fm`, `test/gate.sh:164`). R1 does not compare the file bytes. Evidence:
media R1 60 rows, P4, P8, P17 and P17b. An input must have yuv420p video
(`MEDIA_FORMAT`, `src/media.c:499`). Constant and variable frame rate video
is accepted (M4-R1). Pts keep source timing relative to the first kept
frame, rounded to `1/fr`; the last frame keeps its own duration (M4-R2).
Missing video pts, duplicate source pts, no positive rate, and kept pts
that collide after rounding are `MEDIA_TIMESTAMPS` refusals
(`src/media.c:461`, `:837`). The reference uses `-fps_mode passthrough`.
Audio is optional; when present, it must be fltp with a positive sample
rate (`src/media.c:504`). Its pts keep gaps relative to the first kept
sample, as the C media IO cases verify on mp4 and Matroska sources.

## 6. Cost

N is the clip length (1920x1080, 25 fps), not the program size. The
programs are in `test/media/perf.lang`. `whole` keeps all frames. `chain`
does trim, crop, scale, pad and burnText with two text entries. Each cell is the wall time and
the peak resident memory (MB = 1000000 bytes) from `make perf`.

| Step | s10, 250 frames | s20, 500 frames | s60, 1500 frames |
| --- | --- | --- | --- |
| Build `whole` | 1.27 s, 873 MB | 2.55 s, 885 MB | 6.50 s, 922 MB |
| Build `chain` | 1.04 s, 892 MB | 1.81 s, 904 MB | 5.46 s, 936 MB |
| ffmpeg `whole` | 1.47 s, 880 MB | 2.37 s, 884 MB | 6.61 s, 887 MB |
| ffmpeg `chain` | 0.98 s, 893 MB | 1.83 s, 897 MB | 5.28 s, 901 MB |

The peak grows with the compressed input bytes, not with the decoded
frames. From s10 to s60 the `whole` build grew by 49299456 bytes, and the
input grew by 36231023 bytes. One decoded 1080p yuv420p frame is 3110400
bytes, so a build that holds the frames grows by about 3 MB for each frame.
Not probed: the cost of `check` on a large program.

With the decode window (M5), a cut of 100 frames at the end of s60 (`deep`)
builds in 1.53 s at a peak of 906 MB; the ffmpeg command takes 3.45 s
(commit f28d402, `make perf`). On a 1290 s 1080p recording a cut of 394
frames builds in 4.2 to 4.9 s; at commit 98d3f06 it took 30 to 47 s
(P-M5-5).

With `concat` (M6), `join` of s10 with itself (500 frames) builds in
2.58 s at a peak of 932 MB; the ffmpeg command takes 3.33 s (commit
aae4c44, `make perf`). The real world pair of two cuts (394 and 360
frames) builds in 5.2 s at a peak of 907 MB; the reference takes 8.0 s at
930 MB (P-M6-6, `docs/VALIDATION.md`).

## 7. Parametric families

The grammar lets a family take parameters (`src/front/parser.c:4`). The
only family with a parameter is `Lot (size : Nat)` (`domain/domain.lang:8`).
Its constructor appears in a term: `makeLot 3 250`
(`examples/inventory.lang:9`, gate row `inventory lot` in evals). A
program cannot declare a family (`REFUSE_DATA`,
`src/front/check.c:1519`). `SubtitleTrack` is a definition, not a family:
`List (Prod Interval Text)` (`domain/video.lang:12`). `Video` and `Font`
have no constructor. Their values come only from the inputs of a media
entry (`MEDIA_TYPE`, `src/media.c:413`).

## 8. Sigma projection and eta

`pack`, `witness` and `payload` check (`src/front/check.c:102`). The kernel
accepts Sigma eta (`src/front/eval.c:1114`, `sigmaEta` in
`examples/laws.lang:18`). A wrong swap gives `TYPE_REFL` (gate row
`bad-eta.lang` in `test/check/expect.txt`). No media type uses a Sigma.

## 9. Equality

The front end has `Eq`, `refl`, `symm`, `trans`, `cong` and `transport`
(`src/front/check.c:91`). `examples/equality.lang` uses `transport` and
`cong`, and `test/json/equality.json` is its golden file. For media, two
programs are equal when their outputs give the same framemd5 lines. Of
the 29 media law rows, 27 compare the full lines (`exact`). Two rows
compare only the lines without the hash column (`shape`): `twice` versus
`once`, and `scaleText` versus `textScale` (`test/media/laws.txt:11`,
`:16`). Neither scale composition (P3) nor moving scale past burnText
preserves pixels. Passing these two shape rows does not establish pixel
equality.

## 10. Axiom detection

The host refuses each axiom at check: "an axiom has no proof (rule R2)"
(`REFUSE_AXIOM`, `src/front/check.c:1521`). Each command runs the check
first (`src/main.c:133`), so `build` and `ffmpeg` also refuse an axiom
with exit 1. No command lists the axioms of a file. Gate row: `axiom.lang`
in `test/check/expect.txt`.

## Consequences

1. R1 compares decoded frames over all streams, never the file bytes
   (section 5, P4, P8).
2. The compiler never rewrites a program by scale composition or by
   moving scale past burnText (section 9, P3).
3. Each `drawtext` gets `expansion=none` (B1, B2), a `fontfile` (B3, B4)
   and a `fontsize` (B7), and never `fix_bounds` (B8).
4. The build refuses a font that FreeType cannot open before it makes a
   graph (`MEDIA_FONT`), because ffmpeg uses fontconfig and exits 0 (B3).
5. `STYLE_SIZE` refuses the size 0 (B6). Text at an odd position (P13) and
   text outside the frame (B9) are allowed.
6. Text entries draw in list order (P14). An empty track or an empty Text
   changes no pixel (B5).
7. In process, the build picks the text graph by the absolute frame
   position. The ffmpeg command uses `enable='between(n,R,Q)'` with `n`
   relative to the `drawtext` input (P9, P10, P11).
8. The ffmpeg command escapes text and font paths on two levels (P16).
9. Audio trim counts decoded audio values after the AAC priming skip (P7).
10. Peak memory must stay below 4000000000 bytes at 1080p. The cap ladder
    in `make perf` finds a build that holds frames before the s60 run
    (section 6).
11. The video decode of a cut starts at the last decode key at or before
    the first kept position that splits presentation order, or at packet 0
    if none exists (`SPEC.md` O-3). It sends no more video packets after the
    last kept frame (P-M5-2, P-M5-3). Frames from the selected start through
    the last kept position are checked against the index; buffered frames
    emitted afterward are discarded without a pts check. The audio decode
    stays whole (P-M5-4). A test of the stop needs a decoder without delay:
    with `threads=auto`, frame threading hides a stop one position early on
    the libx264 fixtures (`docs/VALIDATION.md`).
12. `concat` runs the segments in order with one encoder pair. Each
    segment's length is the maximum of its audio end and video mean rate
    end in 1/1000000; the seam is the sum of the preceding lengths. Video
    pts round to `1/fr` of the first segment; audio pts round to
    `1/sample_rate`, with silence up to the unrounded seam (P-M6-1).
    `CONCAT_SHAPE` refuses a sample rate or a
    channel layout difference that ffmpeg would convert (P-M6-2). The plan
    repeats an input used twice (P-M6-4). The packets of all the segments
    stay under the cap (P-M6-6).
