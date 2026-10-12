# tcc-media host

The tcc-media host compiles video-lang programs to mp4 files and to JSON.
A C front end (lexer, parser, checker and evaluator), a JSON writer and a
media back end build with TinyCC into one executable, `build/langc`. The
media back end links the FFmpeg libraries and FreeType. A media build
applies a `Video` entry to input files. A JSON build writes the final state
of each instance in the program. `Video` and `Font` are opaque: a program
cannot make a value of these types, only take one as a parameter.

From the repository root:

```sh
make check
build/langc check examples/inventory.lang
build/langc eval examples/inventory.lang stockTotal
build/langc build examples/inventory.lang -o build/inventory.json
build/langc check test/media/text.lang
```

The build uses TinyCC (`TCC`, default `tcc`). Validation uses TinyCC
0.9.28rc on AArch64 Darwin. `make check-clang` also compiles the sources
with `cc -Wall -Wextra -Wswitch-enum -Werror -fsyntax-only` (`CC`, default
`cc`). The gate uses POSIX `sh`, `awk`, `cmp`, `rg` and Node (for `JSON.parse`).
The build also needs `pkg-config`, FreeType (`freetype2`) and the FFmpeg libraries: `libavfilter`,
`libavformat`, `libavcodec`, `libswscale`, `libswresample` and `libavutil`.
The tested FFmpeg version is 9.0.2 from `ffmpeg-full`, with
`FFMPEG_PREFIX=/opt/homebrew/opt/ffmpeg-full`. Override `FFMPEG_PREFIX`
with the installation prefix to use its libraries and CLI together.

## Commands

| Command | Result |
|---|---|
| `langc check PROG` | Checks the program. Prints `ok`. |
| `langc eval PROG NAME [ARGS...]` | Applies the definition `NAME` to the literal arguments and prints the normal form. A Nat overflow prints `trap`. |
| `langc build PROG [-o OUT]` | Checks the program, evaluates each instance and writes one JSON document to stdout, or to `OUT` |
| `langc build PROG MAIN IN... -o OUT` | Applies the Video definition `MAIN` to the input files and encodes the result to `OUT` (mp4: libx264 and aac with the options of the ffmpeg CLI). A failed or refused build keeps an existing `OUT` and creates no new output. |
| `langc ffmpeg PROG MAIN IN... -o OUT` | Prints the ffmpeg command line that makes the same cut. The gate runs it to make the reference output (R1). |

Exit 0 is success. Exit 1 is a refused program. Exit 2 is a usage or IO
error. A refusal writes one line to stderr: `langc: CODE: NAME: message`.
`langc build` makes the full document in memory before it writes. Thus a
refused build writes nothing to stdout and makes no `OUT` file.

## The media build

The media verbs take an entry `MAIN` whose type ends in `Video`. Each
parameter of `MAIN` is a `Video` or a `Font`. The input files follow the
order of the parameters: an mp4 file for a `Video`, a TrueType file for a
`Font`.

The host evaluates the entry to a plan of steps: `trim`, `scale`, `crop`,
`pad` and `burnText`. A `trim` also cuts the audio. Then the host decodes
the input, applies each step and encodes the result with libx264 and aac,
with the options of the ffmpeg CLI. The audio stream is optional. Constant
and variable frame rate sources are accepted. Video pts keep the source
timing relative to the first kept frame, rounded to the encoder time base
`1/fr` (M4-R2). The last frame keeps its own duration. Audio pts keep gaps
relative to the first kept audio sample. The host refuses missing video
pts, duplicate pts, no positive rate, or kept pts that collide after
rounding (`MEDIA_TIMESTAMPS`).

`langc ffmpeg` prints one ffmpeg command for the same plan: a `-vf` chain
of `trim`, `scale`, `crop`, `pad` and `drawtext` filters, and a `-af`
chain, with `-fps_mode passthrough`. The gate runs this command with the
ffmpeg of `FFMPEG_PREFIX`. Then
it compares the decoded frames of the two outputs with framemd5 over all
the streams (ruling R1). `SPEC.md` section 7 lists the media refusal codes.

The host keeps the demuxed packets of the input in memory, not the decoded
frames (O-3). Thus memory grows with the compressed size of the input. At
1920x1080, a 60 second input has a peak resident size below 1 GB
(`make perf`, `docs/VALIDATION.md`).

## The JSON document

```json
{"video-lang":1,"instances":[{"name":"stockTotal","type":"Nat","value":70}]}
```

The key is the language name and the value is the format version, 1.
The generator reserves the name `instances` for this host to keep the two
document keys distinct.
`instances` holds one object for each instance, in source order. An
instance is a definition of the program (not of `domain/`) whose type is
data. These definitions are checked, but they are not instances:

- functions (a Pi type, dependent or not);
- types and families (a universe type);
- equality proofs (an `Eq` type);
- the definitions in `domain/domain.lang`.

`type` is the normal form of the definition type in source syntax, for
example `Sigma (n : Nat) (Eq Nat n 1200)`. Binder names are freshened when
needed to preserve scope. The complete text must fit a 4096-byte buffer;
the build refuses a type that exceeds this size or the printer depth of
200, rather than emitting an abbreviated type.
`value` is the evaluated value. Values carry no types, so the writer walks
the type and the value together (`src/json.c`). The fuel of the evaluator
starts again for each instance.

| Type | JSON value |
|---|---|
| `Nat` | A number: the full unsigned 64-bit value in decimal |
| `Flag` | `true` or `false` |
| `Unit` | `{}` |
| `Prod A B` | `{"first": a, "second": b}` |
| `Sum A B` | `{"inl": a}` or `{"inr": b}` |
| `Option A` | `null` or the value. When `A` is an `Option` or an `Eq` type, `some v` is `{"some": v}`. |
| `List A` | An array |
| `Sigma (x : A) B` | `{"witness": w, "payload": p}`. The payload type is `B` at the witness. |
| `Eq A x y` | `null` |
| A family whose constructors have no fields | The constructor name, as a string |
| A family with one constructor | An object of the fields |
| Any other family | `{"tag": "name", ...}` with the fields of that constructor |

An indexed family uses the same rules. Its index is in `type` (for
example `Lot 3`), not in `value`.

`langc build` refuses a program with these codes:

| Code | Cause |
|---|---|
| `JSON_VALUE` | An instance holds a function, a type or a stuck term |
| `JSON_DEPTH` | An instance nests deeper than 2000 levels. A list spine does not count. |
| `JSON_TYPE` | The complete instance type exceeds the printer's size or depth limit |
| `EVAL_OVERFLOW` | A Nat operation in an instance overflows |

The check and evaluation codes are the same as in `langc check` and
`langc eval`. `test/parse`, `test/check` and `test/emit` hold a program
for each refusal.

`langc eval` also refuses an incomplete normal form with `EVAL_PRINT`
(a 65536-byte buffer and the same printer depth limit).

## The domain

`domain/domain.lang` holds the inventory definitions of `examples/` and the
alias `Position`. `domain/video.lang` holds the video-lang definitions
`Filter`, `idFilter`, `compose` and `SubtitleTrack`. The other video-lang
types and operations are core names of the host. The Makefile joins them into
`build/domain-all.lang`, and `gen/embed.c` embeds that file in the
executable at build time. `domain/README.md` tells how to change them.

## Limits

| Limit | Value | Source |
|---|---|---|
| Source size | 1 MiB | `src/front/front.h:7` |
| Arena | 1 GiB | `src/main.c:11` |
| Media plan steps | 64 | `src/media.c:34` |
| Text entries in all `burnText` steps | 64 | `src/media.c:35` |
| Entry parameters | 64 | `src/media.c:36` |
| ffmpeg filter chain text | 65536 bytes | `src/media.c:37` |
| Parser nesting | 1000 | `src/front/parser.c:16` |
| Checker depth | 2000 | `src/front/check.c:10` |
| Evaluator depth | 2000 | `src/front/core.h:16` |
| Evaluator fuel, for each instance | 20,000,000 steps | `src/front/core.h:17` |
| JSON nesting | 2000 | `src/json.h:15` |
| `type` text | 4096 bytes | `src/json.c:7` |
| Type and normal-form printer depth | 200 | `src/front/eval.c` |

## Media interval primitives

`Position` is an alias for `Nat`. `interval lo hi : Interval` describes a
closed span, including both endpoints. Concrete bounds with `lo > hi` are
refused with `INTERVAL_RANGE` during checking. Bounds that depend on a
function argument are checked when the function is applied.

`intersect i j : Option Interval` returns the common closed span, or
`none` when the intervals are disjoint. Two spans that share an endpoint
intersect in a singleton interval.

An `Interval` instance builds to `{"lo":1,"hi":3}` for `interval 1 3`.
The existing JSON rules for lists and options apply to intervals too.
An overflowing bound is refused with `EVAL_OVERFLOW` when the JSON writer
needs it, and a refused build writes no document.

## Origin

The tcc-media kit is a fork of the JSON host kit of lang-template, and it
adds `src/media.c`, `src/media.h` and the verb `ffmpeg`. The JSON host kit
is a fork of the tcc-wasm kit front end. It does not have the
Wasm writer (`wasm.c`), the IR (`ir.h`) or the target interface
(`target.h`). It adds `src/json.c` and `src/json.h`, and `src/main.c` has
the verbs `check`, `eval` and `build`. The front end also fixes dependent
result inference, Product and Sigma eta, Sum bind error-type preservation,
domain name checking, the F14 universe rules, and complete,
scope-preserving value printing. The snapshot below records the original
fork, before these fixes. To find the changes after the fork,
compare the hashes with `shasum -a 256 src/front/*` in the tcc-wasm kit.

SHA-256 of the tcc-wasm kit sources at the fork (paths relative to that
kit):

| Path | SHA-256 |
|---|---|
| `src/front/ast.h` | `5dbe74e54c8207da17a57810624bf049d729f272c73a0765b9fd504fb3f18720` |
| `src/front/base.c` | `d994f71d11a352896212d97ed6edb5de062dc37ad6f31bf927487c4d8e675d5b` |
| `src/front/base.h` | `d36daae15e9a79ae298eadf5f1f9c3687e077c7305e63fad6cc5b477b58cebca` |
| `src/front/check.c` | `526a8207c66f3276c18767c62de05026866e7e8adc30e2c5383a2975c0a57114` |
| `src/front/check.h` | `5d781eb3f52a7a17db10d375c0fb7c5b578bf0527dd358cfe49ebb779d008d06` |
| `src/front/core.h` | `d04aa9412bb0f25d470bd3b91e3a073284b19e89aa90083e18d63a6d468fd9dc` |
| `src/front/eval.c` | `5a044b8935f77614a2321883afc8df7d2b1d745e198b8f861538dc1a61773c55` |
| `src/front/front.c` | `9b7d8890d0854fe989780431521189fabf0a79e624359733f5a90822bbf08c27` |
| `src/front/front.h` | `7eb0b8de605ad7336eac1bffc9b65587d7113b2e3849a95a9e6adf75fd12989a` |
| `src/front/lexer.c` | `6812190d933fa5dfb1ad60c9d40a46aeb460aab5342feed6c4697fe31a2425bc` |
| `src/front/lexer.h` | `85046ffc1f76878b8dc0102db08458a59fef914d7951462c9e7bf590cb5db314` |
| `src/front/parser.c` | `6220dcc67e0fc4782d60cf9516bae979d2d4eb3c5eb3e047d92b85806b0bb02d` |
| `src/front/parser.h` | `e9caa8e0dbcfc716b572fa754daa3023ee1a55fb16ff77fb0235c97a49adaa1c` |
| `src/ir.h` | `13021f895667552ec1d527267258e8a3437cadde74744a348e1219dff6d773b9` |
| `src/main.c` | `2294beb677fb4c208811937bbf48f1570911de6e52a6068874e90b3489b211ba` |
| `src/target.h` | `077dff69391f69dbd4623858553dee7b5547a8c1edc0c75660e0aede9b15e429` |
| `src/wasm.c` | `4f69fba5ea1f335e95c7f030d970c4e89da84ee13c57d95879137cf96d8235bd` |

`formers/tcc-media.md` gives the status of each former.
`docs/host/CAPABILITY.md` and `probe/CAPABILITY.md` give the host facts.
