# video-lang status

The build implements the milestones K0, M1, M2, M3 and M4 of `SPEC.md`
section 10. M4 accepts variable frame rate sources and keeps their timing.
M3 is the documentation and the 1080p performance checks. The
milestone wide mutant sweep is not in the M3 scope (ruling M3-R1). No other
milestone is open.

## Implemented

- The 11 core types of `SPEC.md` section 5, for example `Video`,
  `Interval`, `Resolution`, `Rect`, `PadSpec`, `Text`, `Font` and `Style`.
- The 13 core operations of `SPEC.md` section 6, for example `interval`,
  `intersect`, `trim`, `scale`, `crop`, `pad`, `style` and `burnText`. A
  `trim` also cuts the audio (ruling R3).
- The domain definitions `Filter`, `idFilter`, `compose` and
  `SubtitleTrack` (`domain/video.lang`).
- The verbs `check`, `eval`, `build` (JSON and media) and `ffmpeg`.
- Constant and variable frame rate video, with source timing relative to
  the first kept frame and `-fps_mode passthrough` (M4-R1, M4-R2).
- R1 on 40 media programs in `make check`, and on 6 runs at 1920x1080 in
  `make perf`.
- The status of each type former (F1 to F15) is in `formers/tcc-media.md`.

## Remaining work

1. None in the scope of K0 to M4. A new milestone needs a USER ruling
   first.

## Known limits

- A media plan has at most 64 steps and 64 entry parameters
  (`src/media.c:34`, `:36`).
- All the `burnText` steps together hold at most 64 text entries, because
  the in-process text mask is one 64 bit word (`src/media.c:35`).
- The host refuses video without a positive frame rate or a pts for each
  frame, duplicate source pts, and kept frames that round to one output
  pts (`MEDIA_TIMESTAMPS`).
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
`src/media.c` is the media back end: the plan, the libavfilter graph, the
text mask, the encoder and the ffmpeg command text. `src/main.c` holds the
verbs.

`test/gate.sh` checks the examples, the parse and check refusals, the
evaluations, the JSON goldens, the media R1 cases, the laws and the media
refusals. `test/mediatool.c` checks the media plan and its limits.
`test/mediareview.c` checks the media IO and the timestamps.
`test/mediaperfcheck.c` checks the frame comparison of `make perf`.
`test/mediaperf.c` is the `make perf` runner.

A program cannot make a `Video` or a `Font` value. A `Style` exists only
under a function that takes a `Font`. A media entry takes only `Video` and
`Font` parameters.
