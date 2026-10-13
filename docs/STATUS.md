# video-lang status

The build implements the milestones K0, M1, M2, M3, M4, M5 and M6 of
`SPEC.md` section 10. M6 adds `concat`: a plan of segments, the seam rule
of the ffmpeg `concat` filter and the `CONCAT_SHAPE` refusal (M6-R1,
M6-R2). M5 decodes the video of a cut from a safe key packet
at or before the first kept position, or from packet 0, and stops sending
video packets after the last kept frame (M5-R1).
M4 accepts variable frame rate sources and keeps their timing.
M3 is the documentation and the 1080p performance checks. The
milestone wide mutant sweep is not in the M3 scope (ruling M3-R1). No other
milestone is open.

## Implemented

- The 11 core types of `SPEC.md` section 5, for example `Video`,
  `Interval`, `Resolution`, `Rect`, `PadSpec`, `Text`, `Font` and `Style`.
- The 14 core operations of `SPEC.md` section 6, for example `interval`,
  `intersect`, `trim`, `scale`, `crop`, `pad`, `style`, `burnText` and
  `concat`. A
  `trim` also cuts the audio (ruling R3).
- The domain definitions `Filter`, `idFilter`, `compose` and
  `SubtitleTrack` (`domain/video.lang`).
- The verbs `check`, `eval`, `build` (JSON and media) and `ffmpeg`.
- Constant and variable frame rate video, with source timing relative to
  the first kept frame and `-fps_mode passthrough` (M4-R1, M4-R2).
- R1 on 60 media programs in `make check`, and on 8 runs at 1920x1080 in
  `make perf`.
- The status of each type former (F1 to F15) is in `formers/tcc-media.md`.

## Remaining work

1. None in the scope of K0 to M6. A new milestone needs a USER ruling
   first.

## Known limits

- A media plan has at most 64 steps over all segments, 64 segments and 64
  entry parameters (`src/media.c:40`, `:41`, `:43`).
- All the `burnText` steps together hold at most 64 text entries, because
  the in-process text mask is one 64 bit word (`src/media.c:42`).
- The host refuses video without a positive frame rate or a pts for each
  frame, duplicate source pts, and kept frames that round to one output
  pts (`MEDIA_TIMESTAMPS`).
- The video decode of a cut starts at the last decode key at or before the
  first kept position that splits presentation order (for H.264, an IDR
  picture); without one, it starts at packet 0 (`SPEC.md` O-3,
  `src/media.c:598`). A frame from that start through the last kept position
  with a pts that differs from the indexed pts is a refusal (`MEDIA_DECODE`,
  `src/media.c:1708`). Buffered frames emitted afterward are discarded
  without a pts check (`src/media.c:1433`).
- `CONCAT_SHAPE` is stricter than ffmpeg: the operands of a `concat` must
  agree in sample rate and channel layout, which ffmpeg converts itself
  (P-M6-2). Different frame rates are accepted; the output keeps the time
  base `1/fr` of the first input, so a second operand with a higher frame
  rate whose frames round to one tick is a `MEDIA_TIMESTAMPS` refusal.
- The design law "scale is a functor" is false on pixels. Thus the compiler
  does not rewrite a program by this law.
- R1 compares decoded frames. It does not compare container bytes.
- The validation uses one host: AArch64 Darwin, TinyCC 0.9.28rc and
  Homebrew `ffmpeg-full` 9.0.2. The Homebrew `ffmpeg` formula has no
  `drawtext` filter.
- The front end limits (arena 1 GiB, fuel 20,000,000 steps for each
  instance, source 1 MiB) are in `docs/host/README.md`.

## Internal boundaries

The front end is in `src/front/`: `lexer.c`, `parser.c`, `check.c` (the
bidirectional checker), `eval.c` (the evaluator and the printer), `base.c`
(the arena and the diagnostics) and `front.c`. `gen/embed.c` embeds the
domain files in the executable. `src/json.c` is the JSON target.
`src/media.c` is the media back end: the plan (a list of segments), the
libavfilter graph, the text mask, the encoder with the seam rule and the
ffmpeg command text. `src/main.c` holds the
verbs.

`test/gate.sh` checks the examples, the parse and check refusals, the
evaluations, the JSON goldens, the media R1 cases, the laws and the media
refusals. `test/mediatool.c` checks the media plan and its limits.
`test/mediareview.c` checks the media IO, the timestamps, the decode
windows and the concat segments.
`test/mediaperfcheck.c` checks the frame comparison of `make perf`.
`test/mediaperf.c` is the `make perf` runner.

A program cannot make a `Video` or a `Font` value. A `Style` exists only
under a function that takes a `Font`. A media entry takes only `Video` and
`Font` parameters.
