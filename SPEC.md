# video-lang specification

`video-lang` is a working name (O-6). `design/DESIGN.md` (the design) is the
source of the core types and core operations. Section 9 gives the rulings
and the decisions. Rule R1 to R5 are the refusal rules of
`formers/FORMERS.md` section 2. Ruling R1 to R4 are the USER rulings of
section 9.

## 1. Purpose

A video-lang program describes a video edit as a function from input videos
to one output video. The edit uses trim, scale, crop, pad and text burn-in.
The compiler `langc` checks the program, evaluates it and writes the
result. The host is tcc-media: C code that TinyCC builds and that links the
FFmpeg libraries and FreeType. The target is a media file. For the same
program, the compiler also prints an ffmpeg command. The decoded frames of
the two outputs are equal (ruling R1, section 8).

The type formers are F1 to F15 of `formers/FORMERS.md`. The core types and
core operations are only those of the design, with the additions that
ruling R4 (crop and pad) and ruling M2-R1 (`Font`) make.

| Command | Result |
|---|---|
| `langc check PROG` | Prints `ok`, or a refusal |
| `langc eval PROG NAME [ARGS...]` | Prints the normal form of NAME |
| `langc build PROG [-o OUT]` | Writes the JSON document of the instances (section 7) |
| `langc build PROG MAIN IN... -o OUT` | Writes the media file of the entry MAIN |
| `langc ffmpeg PROG MAIN IN... -o OUT` | Prints the ffmpeg command for the same program. Writes no media |

The exit code is 0 for a result, 1 for a refusal and 2 for a usage or IO
error (`src/main.c:2`, commands at `src/main.c:33-44`).

## 2. Programs

A program is a sequence of definitions:

```
def NAME : TYPE := TERM
```

A definition can use only the definitions before it. The compiler refuses
these forms at check time:

| Form | Code | Rule |
|---|---|---|
| `family` | `REFUSE_DATA` (`src/front/check.c:1519`) | R1: only `domain/` declares a family |
| `axiom` | `REFUSE_AXIOM` (`src/front/check.c:1521`) | R2 |
| `def rec`, or a use of this or a later definition | `REFUSE_REC` (`src/front/check.c:1523`, `:1128`) | R3 |
| A core, domain or reserved name (`Bool`), or a name used twice | `REFUSE_NAME` (`src/front/check.c:1382-1414`) | R4 |

Thus a program cannot add a data type, an unproved fact or general
recursion. Recursion comes only from `fold` and `unfold` (F6, F7). The
compiler writes the target. A program cannot.

The build joins `domain/domain.lang` and `domain/video.lang` into
`build/domain-all.lang` and puts it in the compiler (`Makefile:30-35`).
These definitions come before each program:

- `def Position : Type 0 := Nat` (`domain/domain.lang:23`).
- `Filter`, `idFilter`, `compose` and `SubtitleTrack`
  (`domain/video.lang:8-12`).
- The families `Color`, `Item`, `Stack` and `Lot` and their definitions
  (`domain/domain.lang:4-19`). The former tests use them.

A text literal is `"..."` with UTF-8 bytes, at most 256 bytes
(`src/front/lexer.h:30`). The lexer refuses a control byte, a bad escape,
invalid UTF-8 or a missing end quote (`TEXT_CHAR`).

Other check time codes: `LEX_NAT_RANGE`, `LEX_CHAR`, `PARSE_EXPECT`,
`PARSE_DEPTH`, `NAME_UNKNOWN`, `TYPE_MISMATCH`, `TYPE_NOT_FUNCTION`,
`TYPE_INFER`, `TYPE_REFL`, `TYPE_ARITY`, `TYPE_UNIVERSE`, `CHECK_DEPTH`.
The value codes of section 5 (`INTERVAL_RANGE`, `FRAME_SIZE`,
`FRAME_OFFSET`, `PAD_COLOR`, `STYLE_SIZE`, `STYLE_COLOR`, `STYLE_POS`) apply
at check time when the field is a literal, and at evaluation otherwise
(`src/front/check.c:899-904`).

A media entry returns `Video`. Each parameter has type `Video` or `Font`
(`MEDIA_TYPE`, `src/media.c:401-413`). The command line binds IN... to the
parameters in order (`MEDIA_ARGS`). A program contains no file path (O-1).

## 3. Type formers

The type formers are F1 to F15 of `formers/FORMERS.md`. This language uses
the tcc-media column of the realization matrix. `formers/tcc-media.md` gives
the host form and the evidence of each former. Each former is DONE. The
table gives the restrictions that the status cells name.

| ID | Status on tcc-media | Effect on this language | Open item |
|---|---|---|---|
| F7 | DONE into `List`, with a `Nat` step limit | Each `unfold` takes a step limit | None |
| F8 | DONE over `Option` and `List` | A test returns `Flag` | None |
| F14 | DONE without cumulativity | A type in `Type 0` is not a type in `Type 1` | None |
| F15 | DONE in `domain/` only | A program cannot declare a family (rule R1) | None |

## 4. Structures

- **Monad**: `pure`, `map`, `bind` on `Option`, `Sum E` and `List`. The
  design type `Maybe` is `Option`, for example the result of `intersect`.
- **Algebra**: `fold` on `Nat`, `List A` and each family in `domain/`.
  `unfold` into `List`, with a `Nat` step limit. A `SubtitleTrack` is a
  `List`, so `fold` and `filter` apply to it.
- **Filterable**: `filter` on `Option` and `List`.
- **Filter monoid**: `idFilter` and `compose` are definitions in
  `domain/video.lang`, not an instance. Their laws hold by evaluation (D1).

## 5. Core types

| Type | Meaning (design label) | Definition |
|---|---|---|
| `Video` | Frames at positions, with optional audio (Basic types; ruling R3) | Primitive (`src/front/check.c:127`). A value comes only from an entry input. Probe-forced: the build keeps the demuxed packets and a position index, not the decoded frames (O-3). A `concat` result keeps the packets of each operand (D-M6-7) |
| `Position` | The presentation rank of a frame (Basic types) | `def Position : Type 0 := Nat` (`domain/domain.lang:23`). Positions are absolute (D2) |
| `Interval` | The positions from `lo` to `hi`, both included (Basic types) | Primitive `interval lo hi` (`src/front/check.c:128-129`). `INTERVAL_RANGE` when `lo > hi` |
| `Resolution` | An output width and height (Spatial scale) | Primitive `res w h` (`src/front/check.c:133`). `FRAME_SIZE`: each value is even, from 2 to 16384 |
| `Rect` | A crop window `x y w h` (ruling R4) | Primitive `rect` (`src/front/check.c:134`). `FRAME_SIZE`; `FRAME_OFFSET` when `x` or `y` is odd |
| `PadSpec` | An output size, an offset and a fill color (ruling R4) | Primitive `padSpec w h x y color` (`src/front/check.c:135`). `FRAME_SIZE`, `FRAME_OFFSET`; `PAD_COLOR` above 0xFFFFFF |
| `Text` | UTF-8 text (Text / subtitle burn-in) | Primitive, written as a literal (`src/front/check.c:143`). `TEXT_CHAR` |
| `Font` | A font file (ruling M2-R1) | Primitive with no constructor (`src/front/check.c:144`). A value comes only from an entry input |
| `Style` | The font, size, color and position of text (Text / subtitle burn-in) | Primitive `style` (`src/front/check.c:145`). `STYLE_SIZE` outside 1 to 1024, `STYLE_COLOR` above 0xFFFFFF, `STYLE_POS` above 16384 |
| `SubtitleTrack` | Timed text entries (Text / subtitle burn-in) | `def SubtitleTrack : Type 0 := List (Prod Interval Text)` (`domain/video.lang:12`) |
| `Filter` | A function from video to video (Sequential composition) | `def Filter : Type 0 := (v : Video) -> Video` (`domain/video.lang:8`) |

## 6. Core operations

| Operation | Type | Meaning (design label) |
|---|---|---|
| `interval` | `Nat -> Nat -> Interval` | The positions from `lo` to `hi` (Basic types) |
| `trim` | `Interval -> Video -> Video` | Keeps the frames at the positions of the interval. The positions do not change (D2). The audio follows (ruling R3) (Operations and their denotations) |
| `intersect` | `Interval -> Interval -> Option Interval` | The common positions, or `none` (Operations and their denotations) |
| `res` | `Nat -> Nat -> Resolution` | A resolution (Spatial scale) |
| `scale` | `Resolution -> Video -> Video` | Scales each frame, bicubic (Spatial scale) |
| `rect` | `Nat -> Nat -> Nat -> Nat -> Rect` | A crop window (ruling R4) |
| `crop` | `Rect -> Video -> Video` | Keeps the window of each frame (ruling R4) |
| `padSpec` | `Nat -> Nat -> Nat -> Nat -> Nat -> PadSpec` | A pad size, offset and color (ruling R4) |
| `pad` | `PadSpec -> Video -> Video` | Puts each frame on a larger frame of one color (ruling R4) |
| `style` | `Font -> Nat -> Nat -> Nat -> Nat -> Style` | A text style (Text / subtitle burn-in) |
| `burnText` | `Style -> List (Prod Interval Text) -> Video -> Video` | Draws each entry on the frames of its interval. Entries draw in list order (P14) (Text / subtitle burn-in) |
| `concat` | `Video -> Video -> Video` | The first video followed by the second. The positions of the result are its ranks: `p < |a|` is frame `p` of `a`, else frame `p - |a|` of `b`. The audio follows the frames (Concatenation; M6-R2) |
| `idFilter` | `Filter` | The identity (Sequential composition) |
| `compose` | `(f : Filter) -> (g : Filter) -> Filter` | `f` after `g` (Sequential composition) |

`scale`, `crop`, `pad` and `burnText` do not change the audio (ruling R3).
`concat` joins the audio of its operands in order (D-M6-1). `concat` is
associative; `scale`, `crop` and `pad` distribute over it; `trim` splits at
the seam; `burnText` does not distribute (D-M6-4). The compiler never
rewrites a program by these laws (D1); the gate tests them by evaluation.
`encode` is not an operation: the `build` command encodes (O-1, O-5). These
design items are not in the language: `Frame` (a value of the runtime
only), the composition of two resolutions (D1), an encode specification
(O-5) and audio operations.

## 7. Target and instance encoding

`langc build PROG MAIN IN... -o OUT` evaluates MAIN to a list of segments,
each a chain of steps over one input; a program without `concat` is one
segment (D-M6-5). It runs the steps in process with the FFmpeg libraries and
encodes with the default encoders of the container, libx264 and aac for
`.mp4` (O-5). It writes a temporary file and renames it to OUT only when
the encode succeeds (`src/media.c:1645`, `:1701`). Positions start again
from 0 at encode (O-2). The video keeps the source timing relative to its
first kept frame, rounded to the encoder time base (M4-R2); at a `concat`
seam the pts continue from the end of the segment before it (D-M6-2). An
output with no frame is a refusal, and no file is written (D3). `langc
ffmpeg` prints `ffmpeg -i IN -vf VF [-af AF] -fps_mode passthrough OUT` for
one segment, and for K segments `ffmpeg -i IN0 -i IN1 ... -filter_complex
'[0:v]VF0[v0];[0:a]AF0[a0];...[v0][a0][v1][a1]concat=n=K:v=1:a=1[v][a]' -map
'[v]' -map '[a]' -enc_time_base:v FR_DEN:FR_NUM -fps_mode passthrough OUT`
(`a=0` and no audio labels when the inputs have no audio). `FR_NUM/FR_DEN`
is the guessed frame rate of the first segment (`src/media.c:1054`,
`:1094`). The proofs and the types
erase: only the steps reach the output.

The build refuses with these codes (`src/media.c`):

| Code | Cause |
|---|---|
| `EVAL_ENTRY` | No definition MAIN |
| `MEDIA_TYPE`, `MEDIA_ARGS` | The entry type is wrong, or the input count is wrong |
| `MEDIA_SHAPE` | The result is not a chain of `trim`, `scale`, `crop`, `pad`, `burnText` and `concat` over `Video` inputs |
| `MEDIA_LIMIT` | More than 64 steps over all segments, 64 segments or 64 entry parameters, or a filter chain above 65536 bytes |
| `CONCAT_SHAPE` | The operands of a `concat` differ in frame size, pixel format, audio presence, sample rate or channel layout, or both have known sample aspect ratios that differ (D-M6-3) |
| `TEXT_LIMIT` | More than 64 text entries in total in the `burnText` steps |
| `INTERVAL_ORDER` | A trim interval with `lo > hi` |
| `MEDIA_INPUT`, `MEDIA_NO_VIDEO` | An input does not open, or has no video stream |
| `MEDIA_FORMAT` | The video is not yuv420p, or the audio is not fltp with a positive rate |
| `MEDIA_TIMESTAMPS` | A video frame has no pts, two video frames have the same pts, the video has no positive frame rate, or two kept frames round to one output pts (O-2); with `concat`, a segment of one frame without audio that is not last |
| `EMPTY_VIDEO` | No frame to encode (D3) |
| `MEDIA_FONT` | FreeType cannot open a `Font` input |
| `CROP_RANGE`, `PAD_RANGE` | The crop window is outside the frame, or the pad frame does not hold the frame |
| `MEDIA_ENCODE`, `MEDIA_DECODE` | The encode fails, or the frame count does not agree with the index |

`langc build PROG [-o OUT]` writes the JSON document
`{"video-lang":1,"instances":[...]}`. An instance is a top-level definition
whose type is a data type (F14). Each instance gives its name, its type and
its value. `docs/host/README.md`, section The JSON document, gives the
encoding table. A definition of an `Eq` type is not an instance, and an
`Eq` value inside a value is `null`.

## 8. Host and target

The host is tcc-media: TinyCC, the FFmpeg libraries and FreeType. The pin
is Homebrew ffmpeg-full 9.0.2 (libavformat 63.1.102). The libraries and the
reference ffmpeg command come from the same pin (ruling R2). Each make
command takes `FFMPEG_PREFIX=/opt/homebrew/opt/ffmpeg-full`. The gate font is
Arial.ttf (ruling M2-R2).

These facts of `probe/CAPABILITY.md` set the design:

- P3: two `scale` steps do not give the frames of one `scale`. Thus the
  compiler never rewrites a program by a law that is false on pixels (D1).
- P5: the stock Homebrew ffmpeg has no `drawtext`. Thus the pin is
  ffmpeg-full.
- P4, P8: the ffmpeg encode and `drawtext` are deterministic.
- B1 to B8: each `drawtext` gets `expansion=none`, a `fontfile` and a
  `fontsize`, and never `fix_bounds`.
- Section 6: at 1080p, the peak resident memory of a build was 873 MB to
  936 MB for 250 to 1500 frames (`make perf`), below the cap of 4000000000
  bytes. Memory grows with the compressed bytes, not with the frames (O-3).

The check on the output is the gate (`test/gate.sh`). For each media case it
runs `langc build` and the `langc ffmpeg` command, then compares the
framemd5 lines of the two outputs over all streams (ruling R1). Of the 29
law rows, 27 compare the full lines and 2 compare the lines without the
hash column (`probe/CAPABILITY.md` section 9). There is no axiom report,
because the compiler refuses `axiom` (section 10 of the probe).

## 9. Open items

- R1. RULED 2026-10-10 (USER): two outputs are equal when the decoded
  frames are identical: `ffmpeg -i OUT -f framemd5 -` gives the same lines
  for each stream, video and audio. The container bytes are not part of the
  gate.
- R2. RULED 2026-10-10 (USER): the burnText reference is Homebrew
  ffmpeg-full. One pin gives the libraries and the reference command.
- R3. RULED 2026-10-10 (USER): audio is part of M1. `trim` also trims the
  audio. The spatial operations and `burnText` do not change the audio.
- R4. RULED 2026-10-10 (USER): the spatial operations are `scale`, `crop`
  and `pad`.
- M2-R1 to M2-R4. RULED 2026-10-10 (USER): a `Font` is a command line
  input; the gate font is Arial.ttf; `Text` is a UTF-8 literal; a track is
  `List (Prod Interval Text)`.
- M3-R1 to M3-R4. RULED 2026-10-10 (USER): M3 is the docs and the 1080p
  cost, and the mutant sweep is not part of M3; make rules generate the
  1080p clips; the peak memory is the maximum resident set size;
  `design/DESIGN.md` keeps " - " in place of its 4 long dashes.
- M4-R1. RULED 2026-10-11 (USER): accept variable frame rate sources.
  Positions stay absolute frame ranks (D2). There is no new core operation;
  a frame rate operation is outside the scope.
- M4-R2. RULED 2026-10-11 (USER): keep the source timing relative to the
  first kept frame. The reference uses `-fps_mode passthrough`, with one
  output frame for each kept position. R1 still compares pts and duration.
- M5-R1. RULED 2026-10-12 (USER): seek before decode. The video decode of
  a cut starts at a safe decode key at or before the first kept position,
  or at packet 0 if none exists (O-3). It sends no more video packets after
  the decoder gives the last kept frame. The R1 reference stays the same.
  The audio decode stays whole (D-M5-3).
- M6-R1. RULED 2026-10-12 (USER): M6 is concatenation: the core operation
  `concat`, a plan of segments, the encode of two or more sources and the
  ffmpeg `concat` filter as the reference. The real world check on the raw
  VFR recording, time based audio positions and the milestone wide mutant
  sweep are not in M6.
- M6-R2. RULED 2026-10-12 (USER): the `design/DESIGN.md` Concatenation
  clause is the D-M6-1 text: `concat a b` is the sequence `a` followed by
  `b`, the positions of the result are its ranks, and the audio follows the
  frames.
- D1. Probe-forced (P3): the compiler never rewrites a program by a law
  that is false on pixels. Exact laws: trim twice, trim by an intersection,
  and each operation with trim. Laws on the frame shape only: scale
  composition, scale moved past burnText, and encode as the identity.
- D2. Positions are absolute ranks. `trim` keeps the source positions. The
  reference command converts them to relative positions.
- D3. An empty intersection gives the empty video. Its encode is a build
  refusal (`EMPTY_VIDEO`), and no file is written.
- D-M6-1. Default (M6-R2): `concat a b` is `a` followed by `b`. A position
  `p < |a|` is frame `p` of `a`; `p >= |a|` is frame `p - |a|` of `b`. The
  audio of `a`, then the audio of `b`; the last frame of `a` keeps the audio
  of `a` to the end of its stream (O-4).
- D-M6-2. Default: each segment's length is the maximum of its audio end
  (the last pts plus the sample duration) and its video mean rate end
  (the last pts times n/(n - 1) for n >= 2, or 0 for one frame), relative
  to its start, in 1/1000000. The seam is the sum of the preceding segment
  lengths on that clock. Video pts round to the `1/fr` encoder grid of
  the first segment; audio pts round to `1/sample_rate`. Short audio is
  padded with silence up to the unrounded seam (P-M6-1).
- D-M6-3. Default: `CONCAT_SHAPE` refuses operands that differ in frame
  size (after the steps under the `concat`), pixel format, audio presence,
  sample rate or channel layout, or whose known sample aspect ratios
  differ. An unknown sample aspect ratio does not cause this refusal;
  `unsafe` stays off. Different frame rates are accepted (M4-R1). ffmpeg converts the rate
  and the channels itself (P-M6-2), so this refusal is stricter than the
  reference.
- D-M6-4. Default (D1: the compiler never rewrites; the gate tests the laws
  by evaluation and R1): `concat` is associative; `scale`, `crop` and `pad`
  distribute over it; `trim` splits at the seam, each side in the positions
  of its own operand, and a side that keeps no frame is the empty video.
  `burnText` does not distribute: its track is indexed by the positions of
  the result (P-M6-3).
- D-M6-5. Default: the plan is a list of segments, each one input and its
  steps, in order. A `concat` appends the segment lists of its operands. A
  step after a `concat` is pushed into each segment; a pushed `trim` or
  text entry splits at the segment lengths. Caps: 64 segments and 64 steps
  over all segments (`MEDIA_LIMIT`). The reference is the `-filter_complex`
  command of section 7; one segment prints the one input command, so the
  earlier goldens do not change.
- D-M6-6. Default: a segment with no kept frame is the `EMPTY_VIDEO`
  refusal (D3). There is no unit law.
- D-M6-7. Default (O-3): each segment keeps the packets of its source, and
  the cap of 4 GB applies to the sum. Each segment has its own decode window
  (M5). The same file in two segments opens twice (P-M6-4).
- D-M6-8. Default: the encode runs the segments in order with one encoder
  pair. The video pts and the audio sample clock continue at the seam
  (D-M6-2).
- O-1. Default: a program takes `Video` parameters, and the command line
  binds the files. A program contains no file path. Encode is the boundary
  of the `build` command.
- O-2. Revised by M4-R1 and M4-R2: accept constant and variable frame rate
  sources. Video pts start at 0 (`setpts=PTS-STARTPTS`) and keep the source
  intervals. The encoder time base is `1/fr`, where `fr` is the source rate
  from `av_guess_frame_rate`. Pts round to the nearest tick, with ties up
  (P17b); the last frame keeps its own duration in those ticks (P17).
  Audio pts start at 0 (`asetpts=PTS-STARTPTS`) and keep timestamp gaps.
  Like the ffmpeg CLI, the host subtracts the container start time from
  audio pts before the sample rounding; a gap starts a new sample clock.
  The reference uses `-fps_mode passthrough`. A video without a positive
  rate, a frame without pts, duplicate source pts, or two kept frames that
  round to one output pts is a refusal (`MEDIA_TIMESTAMPS`). The gate
  compares the full framemd5 line over all streams.
- O-3. Default: in memory, a `Video` is the demuxed (compressed) packets
  and the position index. The decoded frames go through the operations one
  at a time at encode. Reason: one decoded 1080p frame is 3.1 MB, so a full
  video does not fit the cap of 4 GB for one run. At encode, the video
  decode starts at the last decode key (for H.264, an IDR picture) at or
  before the first kept position that splits the presentation order: every
  earlier video packet has a lower pts and every later one a higher pts.
  Without such a key it starts at packet 0. It sends no video packet after
  the decoder gives the last kept position. Frames from the selected start
  through the last kept position are checked against the position index;
  buffered frames emitted afterward are discarded without a pts check
  (M5-R1, `src/media.c:598`, `:1401-1439`, `:1613`). With `concat`, each
  segment keeps the packets of its source and has its own decode window;
  the same path opens once for each segment, and the cap applies to the
  sum (D-M6-7).
- O-4. Probe-forced (P7): the audio of frame p is the audio from the time of
  p to the time of p + 1. The last frame keeps the audio to the end of the
  stream. The reference is `atrim` with the same audio positions.
  Known limit: the sample count ignores gaps, so after a gap of G a cut
  keeps audio G later than its first frame, as `atrim` does. With `concat`,
  O-4 holds for each operand: the last frame of the first operand keeps its
  audio to the end of that stream, past the video end when the audio is
  longer; when the audio is shorter, silence fills up to the seam; the
  audio of the next operand starts at the seam (D-M6-1, D-M6-2).
- O-5. Default: encode uses the container defaults of the ffmpeg command
  (libx264 and aac for `.mp4`). There is no encode specification.
- O-6. Default: the name is video-lang.
- O-7. Default: the host kit is tcc-media in lang-template. `bin/new-lang.sh`
  made this repository from it.

## 10. Milestones

| Milestone | Content |
|---|---|
| K0 | Host kit tcc-media; `src/media.c` with demux, position index, `trim` and encode, video and audio; the framemd5 gate. Exit: the trim cases are equal under ruling R1 |
| M1 | This repository from the kit; `domain/video.lang`; `scale`, `crop`, `pad`, `compose`, `idFilter`, `intersect`; the D1 exact laws; the refusals |
| M2 | `burnText` with `Text`, `Font`, `Style` and `SubtitleTrack`, against ffmpeg-full `drawtext` |
| M3 | Hardening: the docs, and the cost at 1080p below 4 GB (ruling M3-R1) |
| M4 | Variable frame rate sources; source timing with `-fps_mode passthrough`; VFR R1 cases, exact laws and timestamp refusals (M4-R1, M4-R2) |
| M5 | Decode window: the video decode of a cut starts at a safe key packet at or before the first kept position, or at packet 0; it stops sending video packets after the last kept frame; frames from the selected start through the last kept position are checked against the position index (M5-R1) |
| M6 | Concatenation: `concat`, a plan of segments with pushed steps, the encode of two or more sources with the seam rule of the ffmpeg `concat` filter, `CONCAT_SHAPE`, and the `-filter_complex` reference (M6-R1, M6-R2, D-M6-1 to D-M6-8) |

Status 2026-10-12: K0, M1, M2, M3, M4, M5 and M6 are DONE. M3 Steps 1 to 4 are DONE.
Step 1 is `make perf`. Step 2 is the host docs. Step 3 is this file and
`formers/`. Step 4 is `README.md`, `docs/host/README.md`, `docs/STATUS.md`
and `docs/VALIDATION.md`. No milestone is open. A new milestone needs a new
ruling.
