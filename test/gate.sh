#!/bin/sh
# The kit gate. `make check` runs it from the kit directory after the build.
set -eu
fail=0
count=0
for prog in examples/*.lang; do
  out=$(build/langc check "$prog" 2>&1) || true
  count=$((count + 1))
  if [ "$out" != "ok" ]; then
    echo "FAIL $prog: $out"
    fail=$((fail + 1))
  fi
done
echo "examples: $count checked, $fail failed"

tmp=${TMPDIR:-/tmp}/langc-gate.$$
mkdir -p "$tmp"
awk 'BEGIN { printf "def deep : Nat := " } { } END {
  for (i = 0; i < 1100; i++) printf "("; printf "1"; for (i = 0; i < 1100; i++) printf ")"; print "" }' </dev/null >"$tmp/deep.lang"

# refusals DIR LABEL: each line of DIR/expect.txt is `FILE CODE`. `langc check`
# must exit 1 and print `langc: CODE: ...` on stderr.
refusals() {
  checked=0
  while read -r file code; do
    path="$1/$file"
    [ "$file" = "deep.lang" ] && path="$tmp/deep.lang"
    status=0
    build/langc check "$path" 2>"$tmp/err" >/dev/null || status=$?
    first=$(head -n 1 "$tmp/err")
    checked=$((checked + 1))
    case "$status:$first" in
      "1:langc: $code: "*) ;;
      *) echo "FAIL $file: want exit 1 and $code, got exit $status: $first"; fail=$((fail + 1)) ;;
    esac
  done <"$1/expect.txt"
  echo "$2 refusals: $checked checked"
}
refusals test/parse parse
refusals test/check check

# A refl refusal shows both normal forms.
build/langc check test/check/bad-refl.lang 2>"$tmp/err" >/dev/null || true
if ! rg -q -F 'the left side normalizes to 1201, the right side normalizes to 1200' "$tmp/err"; then
  echo "FAIL bad-refl.lang: the message does not show both normal forms"
  fail=$((fail + 1))
fi

# Each line of test/eval/expect.txt is `FILE NAME [ARGS] => OUTPUT`. `langc eval`
# must exit 0 and print OUTPUT. A trap also prints `langc: EVAL_OVERFLOW: NAME: ...`
# on stderr.
evals=0
while IFS= read -r line; do
  want=${line#* => }
  set -- ${line%% => *}
  file=$1
  name=$2
  shift 2
  status=0
  got=$(build/langc eval "examples/$file.lang" "$name" "$@" 2>"$tmp/err") || status=$?
  first=$(head -n 1 "$tmp/err")
  want_err=""
  [ "$want" = "trap" ] && want_err="langc: EVAL_OVERFLOW: $name: "
  evals=$((evals + 1))
  case "$status|$got|$first" in
    "0|$want|$want_err"*) ;;
    *) echo "FAIL eval $file $name: want $want, got exit $status: $got $first"; fail=$((fail + 1)) ;;
  esac
done <test/eval/expect.txt
echo "evals: $evals checked"

# eval_refused STATUS CODE NAME [ARGS]: `langc eval examples/entries.lang NAME
# ARGS` must exit STATUS and print `langc: CODE: NAME: ...` on stderr.
eval_refused() {
  want_status=$1
  code=$2
  shift 2
  status=0
  build/langc eval examples/entries.lang "$@" 2>"$tmp/err" >/dev/null || status=$?
  case "$status:$(head -n 1 "$tmp/err")" in
    "$want_status:langc: $code: $1: "*) ;;
    *) echo "FAIL eval $*: want exit $want_status and $code, got exit $status"; fail=$((fail + 1)) ;;
  esac
}
eval_refused 2 EVAL_ARGS addPrice 1
eval_refused 2 EVAL_ARGS addPrice x 1
eval_refused 2 EVAL_ARGS addPrice 18446744073709551616 1
eval_refused 2 EVAL_ARGS pick 2 4 9
eval_refused 1 EVAL_ENTRY nope

status=0
build/langc 2>/dev/null || status=$?
[ "$status" -eq 2 ] || { echo "FAIL usage: want exit 2, got $status"; fail=$((fail + 1)); }
status=0
build/langc check "$tmp/missing.lang" 2>/dev/null || status=$?
[ "$status" -eq 2 ] || { echo "FAIL missing file: want exit 2, got $status"; fail=$((fail + 1)); }
# JSON: `langc build` of each example must equal test/json/NAME.json byte for
# byte, and each golden must parse as JSON.
jsons=0
for prog in examples/*.lang; do
  name=$(basename "$prog" .lang)
  jsons=$((jsons + 1))
  if ! build/langc build "$prog" >"$tmp/out.json" 2>"$tmp/err" || ! cmp -s "$tmp/out.json" "test/json/$name.json"; then
    echo "FAIL build $prog: the output differs from test/json/$name.json"
    fail=$((fail + 1))
  fi
done
if ! node -e 'for (const f of process.argv.slice(1)) JSON.parse(require("fs").readFileSync(f, "utf8"))' test/json/*.json; then
  echo "FAIL: a JSON golden does not parse"
  fail=$((fail + 1))
fi
echo "json: $jsons builds checked"

# Each line of test/emit/expect.txt is `FILE CODE`. `langc build` must exit 1,
# print `langc: CODE: ...` on stderr and print nothing on stdout.
while read -r file code; do
  status=0
  build/langc build "test/emit/$file" >"$tmp/out.json" 2>"$tmp/err" || status=$?
  case "$status:$(head -n 1 "$tmp/err"):$(wc -c <"$tmp/out.json" | tr -d ' ')" in
    "1:langc: $code: "*":0") ;;
    *) echo "FAIL build $file: want exit 1, $code and no stdout, got exit $status"; fail=$((fail + 1)) ;;
  esac
done <test/emit/expect.txt
echo "build refusals: $(wc -l <test/emit/expect.txt | tr -d ' ') checked"

# -o writes the same document; a refused build leaves no file.
build/langc build examples/formers.lang -o "$tmp/o.json" && cmp -s "$tmp/o.json" test/json/formers.json \
  || { echo "FAIL build -o: the file differs"; fail=$((fail + 1)); }
build/langc build test/emit/trap.lang -o "$tmp/no.json" 2>/dev/null || true
[ ! -e "$tmp/no.json" ] || { echo "FAIL build -o: a refused build wrote a file"; fail=$((fail + 1)); }
status=0
build/langc build examples/formers.lang -x 2>/dev/null >/dev/null || status=$?
[ "$status" -eq 2 ] || { echo "FAIL build usage: want exit 2, got $status"; fail=$((fail + 1)); }

# Media (video-lang K0). The pin makes the fixtures at each run; they are never
# committed: av.mp4 (video and audio), v.mp4 (no audio), vfr.mp4 (one dropped
# frame, so no constant frame rate).
kit=$(pwd)
media="$tmp/media"
mkdir -p "$media"
ff() { "$FFMPEG" -nostdin -loglevel error -y "$@"; }
fm() { "$FFMPEG" -nostdin -loglevel error -i "$1" -map 0 -f framemd5 -; }
src=testsrc2=size=320x240:rate=25:duration=2
(cd "$media" &&
  ff -f lavfi -i "$src" -f lavfi -i sine=frequency=440:duration=2 -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest av.mp4 &&
  ff -f lavfi -i "$src" -c:v libx264 -pix_fmt yuv420p v.mp4 &&
  ff -f lavfi -i "$src" -vf "select='not(eq(n,5))'" -fps_mode passthrough -c:v libx264 -pix_fmt yuv420p vfr.mp4) \
  || { echo "FAIL media: the pin did not make the fixtures"; fail=$((fail + 1)); }
set -f

# Each line of test/media/expect.txt is `NAME FRAMES PROG MAIN IN...`. The
# ffmpeg verb must print test/ffmpeg/NAME.cmd, and the pin runs that argv to
# make REF. OURS (langc build) must have FRAMES video frames and the framemd5
# lines of REF for every stream (R1).
while read -r name frames prog entry inputs; do
  (cd "$media" && rm -f ours.mp4 ref.mp4 &&
    line=$("$kit/build/langc" ffmpeg "$kit/test/media/$prog" "$entry" $inputs -o ref.mp4) &&
    [ "$line" = "$(cat "$kit/test/ffmpeg/$name.cmd")" ] &&
    set -- $line && shift && ff "$@" &&
    "$kit/build/langc" build "$kit/test/media/$prog" "$entry" $inputs -o ours.mp4 &&
    fm ours.mp4 >ours.md5 && fm ref.mp4 >ref.md5 && cmp -s ours.md5 ref.md5 &&
    [ "$(rg -c '^0,' ours.md5)" = "$frames" ]) \
    || { echo "FAIL media $name: the argv, REF, OURS or the framemd5 differ"; fail=$((fail + 1)); }
done <test/media/expect.txt
echo "media R1: $(wc -l <test/media/expect.txt | tr -d ' ') checked"

# Each line of test/media/laws.txt is `LEFT RIGHT IN`: the two builds have the
# same framemd5 (D1: trim i (trim i v) = trim i v; trim i (trim j v) =
# trim (i cap j) v).
while read -r left right input; do
  (cd "$media" &&
    "$kit/build/langc" build "$kit/test/media/cut.lang" "$left" "$input" -o left.mp4 &&
    "$kit/build/langc" build "$kit/test/media/cut.lang" "$right" "$input" -o right.mp4 &&
    fm left.mp4 >left.md5 && fm right.mp4 >right.md5 && cmp -s left.md5 right.md5) \
    || { echo "FAIL media law $left = $right on $input"; fail=$((fail + 1)); }
done <test/media/laws.txt
echo "media laws: $(wc -l <test/media/laws.txt | tr -d ' ') checked"

# Each line of test/media/refuse.txt is `CODE STATUS PROG MAIN IN...`: build and
# ffmpeg exit STATUS with `langc: CODE: ` on stderr, no stdout and no file (D3).
while read -r code want prog entry inputs; do
  for verb in build ffmpeg; do
    status=0
    (cd "$media" && rm -f no.mp4 &&
      "$kit/build/langc" $verb "$kit/test/media/$prog" "$entry" $inputs -o no.mp4 >out 2>err) || status=$?
    case "$status:$(head -n 1 "$media/err"):$(wc -c <"$media/out" | tr -d ' '):$([ -e "$media/no.mp4" ] && echo file)" in
      "$want:langc: $code: "*":0:") ;;
      *) echo "FAIL media $verb $code: want exit $want, $code, no stdout and no file, got exit $status"; fail=$((fail + 1)) ;;
    esac
  done
done <test/media/refuse.txt
echo "media refusals: $(wc -l <test/media/refuse.txt | tr -d ' ') checked"

# The media verbs need -o OUT.
for verb in build ffmpeg; do
  status=0
  build/langc $verb test/media/cut.lang main "$media/av.mp4" >/dev/null 2>&1 || status=$?
  [ "$status" -eq 2 ] || { echo "FAIL media $verb usage: want exit 2, got $status"; fail=$((fail + 1)); }
done
set +f

rm -rf "$tmp"

if rg -n '\x{2013}|\x{2014}' . >/dev/null; then
  echo "FAIL: an em-dash or en-dash is in the kit"
  fail=$((fail + 1))
fi
echo "gate: $fail failures"
[ "$fail" -eq 0 ]
