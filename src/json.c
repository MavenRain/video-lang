/* The JSON target. The tcc-json kit only. See json.h. */
#include <stdio.h>
#include <string.h>

#include "json.h"

#define JSON_TYPE_MAX 4096u

/* new-lang.sh replaces the placeholder with the language name. */
static const char LANG_NAME[] = "video-lang";

typedef struct {
  Machine *m;
  const char *def; /* the current instance, for diagnostics */
  char *data;
  size_t len;
  size_t cap;
  unsigned depth;
} Out;

static int put(Out *o, const char *text, size_t n) {
  if (o->len + n + 1 > o->cap) {
    size_t cap = o->cap == 0 ? 4096 : o->cap;
    char *data;
    while (cap < o->len + n + 1)
      cap *= 2;
    data = arena_alloc(o->m->arena, cap);
    if (data == NULL)
      return diag_fail(o->m->diag, "OOM", o->def, "the arena limit is reached");
    if (o->len > 0)
      memcpy(data, o->data, o->len);
    o->data = data;
    o->cap = cap;
  }
  memcpy(o->data + o->len, text, n);
  o->len += n;
  o->data[o->len] = '\0';
  return 1;
}

static int put_text(Out *o, const char *text) {
  return put(o, text, strlen(text));
}

/* A JSON string. Names and printed types are ASCII; escape all the same. */
static int put_string(Out *o, const char *text) {
  char esc[8];
  const char *p;
  int ok = put(o, "\"", 1);
  for (p = text; ok && *p != '\0'; p++) {
    unsigned char ch = (unsigned char)*p;
    int plain = ch >= 0x20 && ch != '"' && ch != '\\';
    if (plain)
      ok = put(o, p, 1);
    else {
      snprintf(esc, sizeof esc, "\\u%04x", ch);
      ok = put(o, esc, 6);
    }
  }
  return ok && put(o, "\"", 1);
}

static int refuse(Out *o, const char *what) {
  return diag_fail(o->m->diag, "JSON_VALUE", o->def, "the instance holds %s, which has no JSON form", what);
}

static int internal(Out *o, const char *what) {
  return diag_fail(o->m->diag, "INTERNAL", o->def, "the JSON printer found %s", what);
}

/* 1 when V is a number or a canonical value. A trap is a compile error. */
static int ready(Out *o, const Value *v) {
  switch (v->kind) {
  case VAL_NAT:
  case VAL_OP:
  case VAL_TEXT:
    return 1;
  case VAL_TRAP:
    return diag_fail(o->m->diag, "EVAL_OVERFLOW", o->def, "the instance value traps (a Nat overflow)");
  case VAL_UNIV:
  case VAL_PI:
  case VAL_SIGMA:
    return refuse(o, "a type");
  case VAL_LAM:
    return refuse(o, "a function");
  case VAL_VAR:
  case VAL_APP:
  case VAL_STUCK:
    return refuse(o, "a stuck term");
  }
  return internal(o, "an unknown value kind");
}

static int value_json(Out *o, const Value *type, const Value *v);

static int sum_json(Out *o, const Value *type, const Value *v) {
  if (val_is(v, OP_INL))
    return put_text(o, "{\"inl\":") && value_json(o, type->args[0], v->args[0]) && put_text(o, "}");
  if (val_is(v, OP_INR))
    return put_text(o, "{\"inr\":") && value_json(o, type->args[1], v->args[0]) && put_text(o, "}");
  return internal(o, "a Sum value that is not inl or inr");
}

/* none is null, so `some` wraps a value that can be null (ledger rule). */
static int option_json(Out *o, const Value *type, const Value *v) {
  const Value *elem = type->args[0];
  int wrap = val_is(elem, OP_OPTION) || val_is(elem, OP_EQ);
  if (val_is(v, OP_NONE))
    return put_text(o, "null");
  if (!val_is(v, OP_SOME))
    return internal(o, "an Option value that is not none or some");
  return (!wrap || put_text(o, "{\"some\":")) && value_json(o, elem, v->args[0]) && (!wrap || put_text(o, "}"));
}

/* Walks the spine, so a long list does not nest the C stack. */
static int list_json(Out *o, const Value *type, const Value *v) {
  const Value *cell = v;
  int first = 1;
  int ok = put_text(o, "[");
  while (ok && val_is(cell, OP_CONS)) {
    ok = (first || put_text(o, ",")) && value_json(o, type->args[0], cell->args[0]) && ready(o, cell->args[1]);
    cell = cell->args[1];
    first = 0;
  }
  if (!ok)
    return 0;
  return val_is(cell, OP_NIL) ? put_text(o, "]") : internal(o, "a List value that is not nil or cons");
}

static int is_enum(const Machine *m, const FamilyInfo *f) {
  uint32_t i;
  for (i = 0; i < f->ctor_count; i++)
    if (m->ctors[f->first_ctor + i].field_count != 0)
      return 0;
  return 1;
}

/* Only constants: the name. One constructor: an object of the fields.
   Otherwise: an object with "tag" and the fields. A field type is in the
   context of the family parameters, as in check.c. */
static int family_json(Out *o, const Value *type, const Value *v) {
  const FamilyInfo *f;
  const CtorInfo *ci;
  const Env *env = NULL;
  uint32_t i;
  int ok;
  if (type->inst >= o->m->family_count || !val_is(v, OP_CTOR) || v->inst >= o->m->ctor_count)
    return internal(o, "a family value that is not a constructor");
  f = &o->m->families[type->inst];
  ci = &o->m->ctors[v->inst];
  if (ci->family != type->inst || v->argc != f->param_count + ci->field_count || type->argc != f->param_count)
    return internal(o, "a constructor of another family");
  if (is_enum(o->m, f))
    return put_string(o, ci->name);
  for (i = 0; i < f->param_count; i++)
    env = env_push(o->m, env, type->args[i]);
  ok = put_text(o, "{");
  if (f->ctor_count != 1)
    ok = ok && put_text(o, "\"tag\":") && put_string(o, ci->name);
  for (i = 0; ok && i < ci->field_count; i++) {
    const FieldInfo *fi = &ci->fields[i];
    const Value *ft = fi->recursive ? type : eval_core(o->m, env, fi->type);
    int lead = i == 0 && f->ctor_count == 1;
    ok = ft != NULL && (lead || put_text(o, ",")) && put_string(o, fi->name) && put_text(o, ":")
      && value_json(o, ft, v->args[f->param_count + i]);
  }
  return ok && put_text(o, "}");
}

static const char *const INTERVAL_KEYS[] = {"lo", "hi"};
static const char *const RES_KEYS[] = {"w", "h"};
static const char *const RECT_KEYS[] = {"x", "y", "w", "h"};
static const char *const PAD_SPEC_KEYS[] = {"w", "h", "x", "y", "color"};

/* A media record (video-lang K0 and M1): one object, one Nat per key. */
static int fields_json(Out *o, const Value *v, Op op, const char *const *keys, uint32_t n, const char *what) {
  const Value *nat = val_make(o->m, OP_NAT, 0, 0, NULL, NULL, NULL);
  uint32_t i;
  int ok;
  if (!val_is(v, op) || v->argc != n)
    return internal(o, what);
  ok = nat != NULL && put_text(o, "{");
  for (i = 0; ok && i < n; i++)
    ok = put_text(o, i == 0u ? "\"" : ",\"") && put_text(o, keys[i]) && put_text(o, "\":")
      && value_json(o, nat, v->args[i]);
  return ok && put_text(o, "}");
}

/* A Text value is a JSON string. The lexer refuses every control byte, so
   only '"' and '\' need an escape. */
static int text_json(Out *o, const Value *v) {
  char one[3];
  uint64_t i;
  int ok;
  if (v->kind != VAL_TEXT)
    return internal(o, "a Text value that is not a literal");
  ok = put_text(o, "\"");
  for (i = 0; ok && i < v->nat; i++) {
    char c = v->name[i];
    int escape = c == '"' || c == '\\';
    one[0] = escape ? '\\' : c;
    one[1] = escape ? c : '\0';
    one[2] = '\0';
    ok = put_text(o, one);
  }
  return ok && put_text(o, "\"");
}

static int op_json(Out *o, const Value *type, const Value *v) {
  char num[24];
  switch (type->op) {
  case OP_TEXT:
    return text_json(o, v);
  case OP_NAT:
    if (v->kind != VAL_NAT)
      return internal(o, "a Nat value that is not a number");
    snprintf(num, sizeof num, "%llu", (unsigned long long)v->nat);
    return put_text(o, num);
  case OP_INTERVAL:
    return fields_json(o, v, OP_MK_INTERVAL, INTERVAL_KEYS, 2u, "an Interval value that is not interval");
  case OP_RESOLUTION:
    return fields_json(o, v, OP_MK_RES, RES_KEYS, 2u, "a Resolution value that is not res");
  case OP_RECT:
    return fields_json(o, v, OP_MK_RECT, RECT_KEYS, 4u, "a Rect value that is not rect");
  case OP_PAD_SPEC:
    return fields_json(o, v, OP_MK_PAD_SPEC, PAD_SPEC_KEYS, 5u, "a PadSpec value that is not padSpec");
  case OP_FLAG:
    if (val_is(v, OP_FLAG_YES))
      return put_text(o, "true");
    return val_is(v, OP_FLAG_NO) ? put_text(o, "false") : internal(o, "a Flag value that is not yes or no");
  case OP_UNIT:
    return put_text(o, "{}");
  case OP_EQ:
    return put_text(o, "null");
  case OP_PROD:
    if (!val_is(v, OP_PAIR))
      return internal(o, "a Prod value that is not a pair");
    return put_text(o, "{\"first\":") && value_json(o, type->args[0], v->args[0]) && put_text(o, ",\"second\":")
      && value_json(o, type->args[1], v->args[1]) && put_text(o, "}");
  case OP_SUM:
    return sum_json(o, type, v);
  case OP_OPTION:
    return option_json(o, type, v);
  case OP_LIST:
    return list_json(o, type, v);
  case OP_FAMILY:
    return family_json(o, type, v);
  case OP_UNIT_VAL:
  case OP_FLAG_YES:
  case OP_FLAG_NO:
  case OP_PAIR:
  case OP_INL:
  case OP_INR:
  case OP_NONE:
  case OP_SOME:
  case OP_NIL:
  case OP_CONS:
  case OP_PACK:
  case OP_REFL:
  case OP_CTOR:
  case OP_FIRST:
  case OP_SECOND:
  case OP_EITHER:
  case OP_OPTION_ELIM:
  case OP_PURE:
  case OP_MAP:
  case OP_BIND:
  case OP_FOLD_NAT:
  case OP_FOLD_LIST:
  case OP_FOLD_FAMILY:
  case OP_UNFOLD:
  case OP_FILTER:
  case OP_WITNESS:
  case OP_PAYLOAD:
  case OP_SYMM:
  case OP_TRANS:
  case OP_TRANSPORT:
  case OP_CONG:
  case OP_NAT_ADD:
  case OP_NAT_SUB:
  case OP_NAT_MUL:
  case OP_NAT_EQ:
  case OP_NAT_LE:
  case OP_FLAG_IF:
  case OP_PROJ:
  case OP_VIDEO:
  case OP_MK_INTERVAL:
  case OP_TRIM:
  case OP_INTERSECT:
  case OP_MK_RES:
  case OP_MK_RECT:
  case OP_MK_PAD_SPEC:
  case OP_SCALE:
  case OP_CROP:
  case OP_PAD:
  case OP_FONT:
  case OP_STYLE:
  case OP_MK_STYLE:
  case OP_BURN_TEXT:
  /* video-lang M6: a concat residual is never an instance (its entry has
     Video parameters). The host reads it as a segment list and refuses a
     shape mismatch as CONCAT_SHAPE (src/media.c plan_of, Step 3). */
  case OP_CONCAT:
    return internal(o, "a type that is not a type former");
  }
  return internal(o, "an unknown operation");
}

/* The payload type is the Sigma body at the witness. */
static int pack_json(Out *o, const Value *type, const Value *v) {
  const Value *payload;
  if (!val_is(v, OP_PACK))
    return internal(o, "a Sigma value that is not pack");
  if (!put_text(o, "{\"witness\":") || !value_json(o, type->dom, v->args[0]))
    return 0;
  payload = closure_apply(o->m, type, v->args[0]);
  return payload != NULL && put_text(o, ",\"payload\":") && value_json(o, payload, v->args[1]) && put_text(o, "}");
}

static int typed_json(Out *o, const Value *type, const Value *v) {
  switch (type->kind) {
  case VAL_OP:
    return op_json(o, type, v);
  case VAL_SIGMA:
    return pack_json(o, type, v);
  case VAL_PI:
    return refuse(o, "a function");
  case VAL_UNIV:
    return refuse(o, "a type");
  case VAL_NAT:
  case VAL_TRAP:
  case VAL_LAM:
  case VAL_VAR:
  case VAL_APP:
  case VAL_STUCK:
  case VAL_TEXT:
    return refuse(o, "a value of a stuck type");
  }
  return internal(o, "an unknown type kind");
}

static int value_json(Out *o, const Value *type, const Value *v) {
  int ok;
  if (!ready(o, v))
    return 0;
  if (o->depth >= JSON_DEPTH_LIMIT)
    return diag_fail(o->m->diag, "JSON_DEPTH", o->def, "the instance nests deeper than %u levels", JSON_DEPTH_LIMIT);
  o->depth++;
  ok = typed_json(o, type, v);
  o->depth--;
  return ok;
}

/* Functions, types, families and proofs are not instances. */
static int is_instance(const DefInfo *d) {
  if (d->origin != ORIGIN_PROGRAM)
    return 0;
  switch (d->type->kind) {
  case VAL_PI:
  case VAL_UNIV:
    return 0;
  case VAL_OP:
    return !val_is(d->type, OP_EQ);
  case VAL_SIGMA:
  case VAL_NAT:
  case VAL_TRAP:
  case VAL_LAM:
  case VAL_VAR:
  case VAL_APP:
  case VAL_STUCK:
  case VAL_TEXT:
    return 1;
  }
  return 1;
}

static int instance_json(Out *o, uint32_t index, char *type_text, int first) {
  Machine *m = o->m;
  const DefInfo *d = &m->defs[index];
  const Value *v;
  o->def = d->name;
  m->def = d->name;
  m->fuel = EVAL_FUEL_STEPS;
  m->depth = 0;
  v = def_value(m, index);
  if (v == NULL)
    return 0;
  if (!value_print(m, NULL, 0, d->type, type_text, JSON_TYPE_MAX))
    return diag_fail(m->diag, "JSON_TYPE", d->name, "the instance type exceeds the printer limits");
  return (first || put_text(o, ",")) && put_text(o, "{\"name\":") && put_string(o, d->name)
    && put_text(o, ",\"type\":") && put_string(o, type_text) && put_text(o, ",\"value\":")
    && value_json(o, d->type, v) && put_text(o, "}");
}

int json_document(Machine *m, const char **text, size_t *len) {
  Out o;
  char *type_text = arena_alloc(m->arena, JSON_TYPE_MAX);
  uint32_t i;
  int first = 1;
  int ok;
  memset(&o, 0, sizeof o);
  o.m = m;
  if (type_text == NULL)
    return diag_fail(m->diag, "OOM", NULL, "the arena limit is reached");
  ok = put_text(&o, "{") && put_string(&o, LANG_NAME) && put_text(&o, ":1,\"instances\":[");
  for (i = 0; ok && i < m->def_count; i++) {
    if (is_instance(&m->defs[i])) {
      ok = instance_json(&o, i, type_text, first);
      first = 0;
    }
  }
  if (!ok || !put_text(&o, "]}\n"))
    return 0;
  *text = o.data;
  *len = o.len;
  return 1;
}
