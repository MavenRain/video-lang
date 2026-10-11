# TinyCC media host capability

This file holds host facts only. Put the facts of your domain in the
`probe/CAPABILITY.md` of your language. Paths are relative to the root of
this repository.

## Toolchain

- TinyCC 0.9.28rc (AArch64 Darwin) builds `build/langc` with
  `-std=c99 -Wall -Werror` (`Makefile:11`).
- The build links libavfilter, libavformat, libavcodec, libswscale,
  libswresample, libavutil and freetype2 with plain `pkg-config` flags
  (`Makefile:7`, P1, P6). `FFMPEG_PREFIX` selects the pin, default
  `/opt/homebrew/opt/ffmpeg-full` (`Makefile:5`). The pin is ffmpeg-full
  9.0.2. The stock Homebrew ffmpeg has no `drawtext` filter (P5).
- `cc -Wall -Wextra -Wswitch-enum -Werror -fsyntax-only` checks the same
  sources and the C test programs (`make check-clang`). Each switch on an
  enum names every case and has no default arm.
- `tcc -run gen/embed.c` turns `build/domain-all.lang` (the joined
  `domain/*.lang` files) into `build/domain.c` at build time. The domain is
  part of the executable.

## Values

- `Nat` is an unsigned 64-bit integer. Literals larger than
  18446744073709551615 are refused by the lexer (`test/parse/nat-range.lang`).
- `natAdd` and `natMul` trap on overflow. `natSub` is truncated at 0.
  A trap is a value: it stops a computation only when the computation
  needs it. `langc eval` prints `trap` (`test/eval/expect.txt`).
  `langc build` refuses an instance that holds a trap (`EVAL_OVERFLOW`).
- A Text literal holds UTF-8 bytes between `"` marks. The lexer refuses
  invalid UTF-8, a control byte and a backslash before any byte other than
  `"` or `\` (`TEXT_CHAR`, `src/front/lexer.c:224`; 9 rows in
  `test/parse/expect.txt`).
- `Video` and `Font` values come only from the inputs of a media entry.
  A media entry returns `Video`; each parameter has type `Video` or
  `Font` (`MEDIA_TYPE`, `src/media.c:243`).

## Limits

- The checker and the evaluator stop at a depth of 2000
  (`src/front/check.c:10`, `src/front/core.h:16`), and the parser at a
  nesting of 1000 (`src/front/parser.c:16`). These limits keep the C stack
  bounded. `test/gate.sh` checks a nesting of 1100 parentheses.
- The evaluator has a fuel of 20,000,000 steps for each instance
  (`src/front/core.h:17`). `langc build` starts the fuel again for each
  instance.
- The front end and the media plan take memory from one arena with a
  limit of 1 GiB (`src/main.c:11`). A run that reaches it stops with
  `OOM`. The libav* libraries take their memory outside the arena.
- Source files are at most 1 MiB (`src/front/front.h:7`).
- A media program has at most 64 steps, 64 entry parameters and a filter
  chain of at most 65536 bytes (`src/media.c:32`, refusal `MEDIA_LIMIT`).
  A media plan has at most 64 text entries in total across all
  `burnText` steps, because the build keeps the active entries in one
  64-bit mask (`src/media.c:195`, refusal `TEXT_LIMIT`).
- At 1080p, the peak resident memory of a build was 873 MB to 936 MB for
  250 to 1500 frames (`make perf`, `probe/CAPABILITY.md` section 6).

## Output

- `langc build PROG [-o OUT]` makes the full JSON document in the arena,
  then writes it. A refused build writes no partial document and no `-o`
  file. The writer walks list spines in a loop, so a long list does not
  use C stack. Other nesting counts against the JSON depth of 2000
  (`src/json.h:15`).
- `langc build PROG MAIN IN... -o OUT` writes a media file. It writes a
  temporary file first and renames it to OUT only when the build succeeds
  (`src/media.c:1057`). The encoders are the defaults of the container
  (`src/media.c:694`).
- `langc ffmpeg PROG MAIN IN... -o OUT` prints the ffmpeg command for the
  same program, and that command writes OUT (`test/gate.sh:179`). The
  decoded frames of the two outputs are equal in all 25 media R1 gate
  cases, over all streams.
