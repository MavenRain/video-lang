/* Regression checks for the media primitives, using the public front end. */
#include <stdio.h>
#include <string.h>

#include "front/check.h"
#include "front/front.h"
#include "json.h"

static unsigned checked;
static unsigned failed;

static void expect(const char *name, int ok, const Diag *diag) {
  checked++;
  if (!ok) {
    fprintf(stderr, "FAIL media %s\n", name);
    if (diag->set) diag_print(diag, stderr);
    failed++;
  }
}

static int load(Arena *arena, Diag *diag, Machine *m, const char *source) {
  DeclList decls;
  return front_load(arena, "media-test.lang", source, strlen(source), &decls, diag)
    && check_program(arena, &decls, m, diag);
}

static void json_case(const char *name, const char *source, const char *want, const char *code) {
  Arena arena;
  Diag diag;
  Machine m;
  const char *text = NULL;
  size_t len = 0;
  arena_init(&arena, (size_t)64 << 20);
  diag_init(&diag);
  int loaded = load(&arena, &diag, &m, source);
  int ok = loaded && json_document(&m, &text, &len);
  expect(name, code != NULL ? !ok && diag.code != NULL && strcmp(diag.code, code) == 0 && text == NULL
      && (strcmp(code, "EVAL_OVERFLOW") == 0 || !loaded)
    : ok && len == strlen(text) && strstr(text, want) != NULL && !diag.set, &diag);
  arena_release(&arena);
}

#define MK_INTERVAL "def mk : (lo : Nat) -> (hi : Nat) -> Interval := fun (lo : Nat) (hi : Nat) => interval lo hi\n"
#define MK_RES "def mk : (w : Nat) -> (h : Nat) -> Resolution := fun (w : Nat) (h : Nat) => res w h\n"
#define MK_RECT "def mk : (x : Nat) -> (y : Nat) -> Rect := fun (x : Nat) (y : Nat) => rect x y 2 2\n"
#define MK_PAD "def mk : (w : Nat) -> (c : Nat) -> PadSpec := fun (w : Nat) (c : Nat) => padSpec w 2 0 0 c\n"

/* SOURCE defines `mk` with two Nat parameters, the arguments 0 and 1 of WANT
   (padSpec: 0 and 4). The fields are checked when mk is applied to X and Y:
   CODE is the refusal, or NULL for a value. */
static void dynamic_case(const char *name, const char *source, uint64_t x, uint64_t y, Op want, const char *code) {
  Arena arena;
  Diag diag;
  Machine m;
  const Value *v = NULL;
  uint32_t second = want == OP_MK_PAD_SPEC ? 4u : 1u;
  arena_init(&arena, (size_t)64 << 20);
  diag_init(&diag);
  int ok = load(&arena, &diag, &m, source);
  if (ok) {
    uint32_t i;
    for (i = 0; i < m.def_count; i++) {
      if (strcmp(m.defs[i].name, "mk") == 0) {
        m.def = "mk";
        v = apply_value(&m, apply_value(&m, def_value(&m, i), val_nat(&m, x)), val_nat(&m, y));
        break;
      }
    }
  }
  expect(name, ok && (code == NULL
    ? v != NULL && val_is(v, want) && v->args[0]->nat == x && v->args[second]->nat == y && !diag.set
    : v == NULL && diag.code != NULL && strcmp(diag.code, code) == 0), &diag);
  arena_release(&arena);
}

int main(void) {
  json_case("interval", "def result : Interval := interval 1 3\n",
    "\"value\":{\"lo\":1,\"hi\":3}", NULL);
  json_case("singleton", "def result : Interval := interval 4 4\n",
    "\"value\":{\"lo\":4,\"hi\":4}", NULL);
  json_case("max endpoint", "def result : Interval := interval 0 18446744073709551615\n",
    "\"value\":{\"lo\":0,\"hi\":18446744073709551615}", NULL);
  json_case("overlap", "def result : Option Interval := intersect (interval 1 3) (interval 2 4)\n",
    "\"value\":{\"lo\":2,\"hi\":3}", NULL);
  json_case("closed endpoints", "def result : Option Interval := intersect (interval 1 2) (interval 2 4)\n",
    "\"value\":{\"lo\":2,\"hi\":2}", NULL);
  json_case("disjoint", "def result : Option Interval := intersect (interval 1 2) (interval 3 4)\n",
    "\"value\":null", NULL);
  json_case("nested intervals", "def result : List Interval := cons (interval 1 3) nil\n",
    "\"value\":[{\"lo\":1,\"hi\":3}]", NULL);
  json_case("nested options", "def result : Option (Option Interval) := some (some (interval 1 3))\n",
    "\"value\":{\"some\":{\"lo\":1,\"hi\":3}}", NULL);
  json_case("literal reversed bounds", "def result : Interval := interval 3 1\n", NULL, "INTERVAL_RANGE");
  json_case("computed reversed bounds", "def result : Interval := interval (natAdd 1 2) 1\n", NULL, "INTERVAL_RANGE");
  json_case("endpoint overflow", "def result : Interval := interval 0 (natAdd 18446744073709551615 1)\n",
    NULL, "EVAL_OVERFLOW");
  /* video-lang M1 */
  json_case("contained", "def result : Option Interval := intersect (interval 0 9) (interval 3 4)\n",
    "\"value\":{\"lo\":3,\"hi\":4}", NULL);
  json_case("computed ends", "def result : Option Interval := intersect (interval (natAdd 1 1) 5) (interval 0 3)\n",
    "\"value\":{\"lo\":2,\"hi\":3}", NULL);
  json_case("compose left identity", "def law : (f : Filter) -> (v : Video) -> Eq Video (compose idFilter f v) (f v) :="
    " fun (f : Filter) (v : Video) => refl\ndef result : Nat := 1\n", "\"value\":1", NULL);
  json_case("compose right identity", "def law : (f : Filter) -> (v : Video) -> Eq Video (compose f idFilter v) (f v) :="
    " fun (f : Filter) (v : Video) => refl\ndef result : Nat := 1\n", "\"value\":1", NULL);
  json_case("compose associative", "def law : (f : Filter) -> (g : Filter) -> (h : Filter) -> (v : Video) ->"
    " Eq Video (compose (compose f g) h v) (compose f (compose g h) v) :="
    " fun (f : Filter) (g : Filter) (h : Filter) (v : Video) => refl\ndef result : Nat := 1\n", "\"value\":1", NULL);
  json_case("compose a spatial filter", "def half : Filter := fun (v : Video) => scale (res 160 120) v\n"
    "def law : (v : Video) -> Eq Video (compose half idFilter v) (scale (res 160 120) v) := fun (v : Video) => refl\n"
    "def result : Nat := 1\n", "\"value\":1", NULL);
  json_case("compose is not commutative", "def law : (f : Filter) -> (g : Filter) -> (v : Video) ->"
    " Eq Video (compose f g v) (compose g f v) := fun (f : Filter) (g : Filter) (v : Video) => refl\n", NULL, "TYPE_REFL");
  json_case("resolution", "def result : Resolution := res 160 120\n",
    "\"value\":{\"w\":160,\"h\":120}", NULL);
  json_case("largest resolution", "def result : Resolution := res 16384 2\n",
    "\"value\":{\"w\":16384,\"h\":2}", NULL);
  json_case("rect", "def result : Rect := rect 40 20 160 120\n",
    "\"value\":{\"x\":40,\"y\":20,\"w\":160,\"h\":120}", NULL);
  json_case("pad spec", "def result : PadSpec := padSpec 400 300 40 30 1056816\n",
    "\"value\":{\"w\":400,\"h\":300,\"x\":40,\"y\":30,\"color\":1056816}", NULL);
  json_case("white pad", "def result : PadSpec := padSpec 2 2 0 0 16777215\n", "\"color\":16777215}", NULL);
  json_case("spatial ops", "def f : (v : Video) -> Video := fun (v : Video) =>"
    " pad (padSpec 4 4 0 0 0) (crop (rect 0 0 2 2) (scale (res 2 2) v))\ndef result : Nat := 1\n",
    "\"value\":1", NULL);
  json_case("odd width", "def result : Resolution := res 161 120\n", NULL, "FRAME_SIZE");
  json_case("zero height", "def result : Resolution := res 160 0\n", NULL, "FRAME_SIZE");
  json_case("width above the limit", "def result : Resolution := res 16386 2\n", NULL, "FRAME_SIZE");
  json_case("odd crop height", "def result : Rect := rect 0 0 160 121\n", NULL, "FRAME_SIZE");
  json_case("zero pad width", "def result : PadSpec := padSpec 0 2 0 0 0\n", NULL, "FRAME_SIZE");
  json_case("computed odd width", "def result : Resolution := res (natAdd 160 1) 120\n", NULL, "FRAME_SIZE");
  json_case("odd crop x", "def result : Rect := rect 1 0 2 2\n", NULL, "FRAME_OFFSET");
  json_case("odd pad y", "def result : PadSpec := padSpec 4 4 0 3 0\n", NULL, "FRAME_OFFSET");
  json_case("pad color above 24 bits", "def result : PadSpec := padSpec 4 4 0 0 16777216\n", NULL, "PAD_COLOR");
  json_case("crop offset overflow", "def result : Rect := rect (natAdd 18446744073709551615 1) 0 2 2\n",
    NULL, "EVAL_OVERFLOW");
  json_case("scale takes a Resolution",
    "def f : (v : Video) -> Video := fun (v : Video) => scale (rect 0 0 2 2) v\n", NULL, "TYPE_MISMATCH");
  json_case("crop takes a Rect",
    "def f : (v : Video) -> Video := fun (v : Video) => crop (res 2 2) v\n", NULL, "TYPE_MISMATCH");
  json_case("pad takes a PadSpec",
    "def f : (v : Video) -> Video := fun (v : Video) => pad (res 2 2) v\n", NULL, "TYPE_MISMATCH");
  dynamic_case("dynamic valid bounds", MK_INTERVAL, 1, 3, OP_MK_INTERVAL, NULL);
  dynamic_case("dynamic reversed bounds", MK_INTERVAL, 3, 1, OP_MK_INTERVAL, "INTERVAL_RANGE");
  dynamic_case("dynamic resolution", MK_RES, 160, 120, OP_MK_RES, NULL);
  dynamic_case("dynamic odd width", MK_RES, 161, 120, OP_MK_RES, "FRAME_SIZE");
  dynamic_case("dynamic rect", MK_RECT, 2, 4, OP_MK_RECT, NULL);
  dynamic_case("dynamic odd crop y", MK_RECT, 2, 3, OP_MK_RECT, "FRAME_OFFSET");
  dynamic_case("dynamic pad color", MK_PAD, 2, 16777215, OP_MK_PAD_SPEC, NULL);
  dynamic_case("dynamic pad color above 24 bits", MK_PAD, 2, 16777216, OP_MK_PAD_SPEC, "PAD_COLOR");
  printf("media regressions: %u checked, %u failed\n", checked, failed);
  return failed == 0 ? 0 : 1;
}
