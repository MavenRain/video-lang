/* Tokens of the DSL. SHARED by the tcc kits. */
#ifndef LANG_FRONT_LEXER_H
#define LANG_FRONT_LEXER_H

#include "front/base.h"

typedef enum {
  TOK_EOF,
  TOK_IDENT,
  TOK_NAT,
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_COLON,
  TOK_DEFINE,   /* := */
  TOK_ARROW,    /* -> */
  TOK_FATARROW, /* => */
  TOK_STAR,
  TOK_BAR,
  TOK_KW_DEF,
  TOK_KW_REC,
  TOK_KW_FUN,
  TOK_KW_FAMILY,
  TOK_KW_AXIOM,
  TOK_KW_TYPE,
  TOK_KW_SIGMA,
  TOK_TEXT      /* video-lang M2: "..." */
} TokKind;

/* The largest Text literal, in bytes after the escapes. */
#define TEXT_BYTES 256u

typedef struct {
  TokKind kind;
  const char *text;
  size_t len;
  uint64_t nat;   /* TOK_NAT: the value. TOK_TEXT: the byte count. */
  const char *bytes; /* TOK_TEXT: the bytes after the escapes, NUL-terminated */
  int line;
  int col;
} Token;

/* The last token is TOK_EOF. */
typedef struct {
  const Token *items;
  size_t count;
} TokenList;

/* SOURCE names the input in diagnostics. Returns 1, or 0 after a diagnostic. */
int lex_source(Arena *arena, const char *source, const char *text, size_t len, TokenList *out, Diag *diag);
const char *tok_kind_name(TokKind kind);

#endif
