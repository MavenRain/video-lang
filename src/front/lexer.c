#include <string.h>

#include "front/lexer.h"

typedef struct {
  const char *text;
  size_t len;
  size_t pos;
  int line;
  int col;
  Token *out;
  size_t count;
  Diag *diag;
  const char *source;
  Arena *arena;
} Lexer;

static const struct {
  const char *word;
  TokKind kind;
} KEYWORDS[] = {
  {"def", TOK_KW_DEF},
  {"rec", TOK_KW_REC},
  {"fun", TOK_KW_FUN},
  {"family", TOK_KW_FAMILY},
  {"axiom", TOK_KW_AXIOM},
  {"Type", TOK_KW_TYPE},
  {"Sigma", TOK_KW_SIGMA},
};

const char *tok_kind_name(TokKind kind) {
  switch (kind) {
    case TOK_EOF: return "end of input";
    case TOK_IDENT: return "a name";
    case TOK_NAT: return "a Nat literal";
    case TOK_LPAREN: return "'('";
    case TOK_RPAREN: return "')'";
    case TOK_COLON: return "':'";
    case TOK_DEFINE: return "':='";
    case TOK_ARROW: return "'->'";
    case TOK_FATARROW: return "'=>'";
    case TOK_STAR: return "'*'";
    case TOK_BAR: return "'|'";
    case TOK_KW_DEF: return "'def'";
    case TOK_KW_REC: return "'rec'";
    case TOK_KW_FUN: return "'fun'";
    case TOK_KW_FAMILY: return "'family'";
    case TOK_KW_AXIOM: return "'axiom'";
    case TOK_KW_TYPE: return "'Type'";
    case TOK_KW_SIGMA: return "'Sigma'";
    case TOK_TEXT: return "a Text literal";
  }
  return "a token";
}

static int is_digit(char c) {
  return c >= '0' && c <= '9';
}

static int is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident_char(char c) {
  return is_ident_start(c) || is_digit(c) || c == '\'';
}

static char peek(const Lexer *lx, size_t ahead) {
  return lx->pos + ahead < lx->len ? lx->text[lx->pos + ahead] : '\0';
}

static void advance(Lexer *lx, size_t width) {
  for (size_t i = 0; i < width && lx->pos < lx->len; i++) {
    int newline = lx->text[lx->pos] == '\n';
    lx->line += newline;
    lx->col = newline ? 1 : lx->col + 1;
    lx->pos++;
  }
}

static Token *push(Lexer *lx, TokKind kind, size_t start, int line, int col) {
  Token *tok = &lx->out[lx->count];
  lx->count++;
  tok->kind = kind;
  tok->text = lx->text + start;
  tok->len = lx->pos - start;
  tok->nat = 0;
  tok->bytes = NULL;
  tok->line = line;
  tok->col = col;
  return tok;
}

/* Skips white space and `--` comments to the end of the line. */
static void skip_trivia(Lexer *lx) {
  for (;;) {
    char c = peek(lx, 0);
    int space = lx->pos < lx->len && (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    int comment = c == '-' && peek(lx, 1) == '-';
    if (!space && !comment) return;
    if (space) {
      advance(lx, 1);
      continue;
    }
    while (lx->pos < lx->len && lx->text[lx->pos] != '\n') advance(lx, 1);
  }
}

static TokKind keyword_kind(const char *text, size_t len) {
  for (size_t i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++) {
    if (strlen(KEYWORDS[i].word) == len && memcmp(KEYWORDS[i].word, text, len) == 0) return KEYWORDS[i].kind;
  }
  return TOK_IDENT;
}

static int lex_ident(Lexer *lx) {
  size_t start = lx->pos;
  int line = lx->line;
  int col = lx->col;
  while (is_ident_char(peek(lx, 0))) advance(lx, 1);
  push(lx, keyword_kind(lx->text + start, lx->pos - start), start, line, col);
  return 1;
}

static int lex_nat(Lexer *lx) {
  size_t start = lx->pos;
  int line = lx->line;
  int col = lx->col;
  uint64_t value = 0;
  int overflow = 0;
  while (is_digit(peek(lx, 0))) {
    uint64_t digit = (uint64_t)(peek(lx, 0) - '0');
    overflow |= value > (UINT64_MAX - digit) / 10u;
    value = value * 10u + digit;
    advance(lx, 1);
  }
  if (overflow) {
    return diag_fail(lx->diag, "LEX_NAT_RANGE", NULL, "%s:%d:%d: a Nat literal is larger than 2^64 - 1", lx->source, line, col);
  }
  if (is_ident_start(peek(lx, 0))) {
    return diag_fail(lx->diag, "LEX_CHAR", NULL, "%s:%d:%d: a letter follows a Nat literal", lx->source, lx->line, lx->col);
  }
  push(lx, TOK_NAT, start, line, col)->nat = value;
  return 1;
}

static int lex_symbol(Lexer *lx) {
  size_t start = lx->pos;
  int line = lx->line;
  int col = lx->col;
  char c = peek(lx, 0);
  char next = peek(lx, 1);
  TokKind kind = TOK_EOF;
  size_t width = 1;
  switch (c) {
    case '(': kind = TOK_LPAREN; break;
    case ')': kind = TOK_RPAREN; break;
    case '*': kind = TOK_STAR; break;
    case '|': kind = TOK_BAR; break;
    case ':':
      kind = next == '=' ? TOK_DEFINE : TOK_COLON;
      width = next == '=' ? 2 : 1;
      break;
    case '-':
      kind = next == '>' ? TOK_ARROW : TOK_EOF;
      width = 2;
      break;
    case '=':
      kind = next == '>' ? TOK_FATARROW : TOK_EOF;
      width = 2;
      break;
    default: break;
  }
  if (kind == TOK_EOF) {
    return diag_fail(lx->diag, "LEX_CHAR", NULL, "%s:%d:%d: unexpected byte 0x%02x", lx->source, line, col, (unsigned)(unsigned char)c);
  }
  advance(lx, width);
  push(lx, kind, start, line, col);
  return 1;
}

/* The length of a UTF-8 sequence that starts with LEAD, or 0 when LEAD cannot
   start one (a continuation byte, an overlong lead C0 or C1, or F5 to FF). */
static size_t utf8_width(unsigned char lead) {
  if (lead >= 0xC2u && lead <= 0xDFu) return 2;
  if (lead >= 0xE0u && lead <= 0xEFu) return 3;
  if (lead >= 0xF0u && lead <= 0xF4u) return 4;
  return 0;
}

/* The range of the second byte after LEAD. The limits refuse overlong forms
   (E0, F0), the surrogates (ED) and code points above U+10FFFF (F4). */
static unsigned utf8_second_low(unsigned char lead) {
  if (lead == 0xE0u) return 0xA0u;
  if (lead == 0xF0u) return 0x90u;
  return 0x80u;
}

static unsigned utf8_second_high(unsigned char lead) {
  if (lead == 0xEDu) return 0x9Fu;
  if (lead == 0xF4u) return 0x8Fu;
  return 0xBFu;
}

/* The length of the valid UTF-8 sequence at the current byte, or 0. peek gives
   0 past the end, so a truncated sequence is not valid. */
static size_t utf8_valid(const Lexer *lx) {
  unsigned char lead = (unsigned char)peek(lx, 0);
  size_t width = utf8_width(lead);
  if (width == 0) return 0;
  unsigned char second = (unsigned char)peek(lx, 1);
  if (second < utf8_second_low(lead) || second > utf8_second_high(lead)) return 0;
  for (size_t i = 2; i < width; i++) {
    unsigned char next = (unsigned char)peek(lx, i);
    if (next < 0x80u || next > 0xBFu) return 0;
  }
  return width;
}

/* The source length of the Text unit at the current byte C: an escape, a
   printable ASCII byte or a UTF-8 sequence. Returns 0 after a diagnostic. */
static size_t text_unit(Lexer *lx, unsigned char c) {
  if (c < 0x20u || c == 0x7Fu) {
    diag_fail(lx->diag, "TEXT_CHAR", NULL, "%s:%d:%d: a Text literal holds the control byte 0x%02x", lx->source, lx->line, lx->col, (unsigned)c);
    return 0;
  }
  if (c == '\\') {
    char next = peek(lx, 1);
    if (next == '"' || next == '\\') return 2;
    diag_fail(lx->diag, "TEXT_CHAR", NULL, "%s:%d:%d: a backslash in a Text literal must come before '\"' or '\\'", lx->source, lx->line, lx->col);
    return 0;
  }
  if (c < 0x80u) return 1;
  size_t width = utf8_valid(lx);
  if (width > 0) return width;
  diag_fail(lx->diag, "TEXT_CHAR", NULL, "%s:%d:%d: a Text literal holds invalid UTF-8 at the byte 0x%02x", lx->source, lx->line, lx->col, (unsigned)c);
  return 0;
}

/* A Text literal (video-lang M2). The escapes are \" and \\ only. The token
   keeps the bytes after the escapes in the arena, NUL-terminated; the byte 0
   cannot occur, because text_unit refuses every control byte. */
static int lex_text(Lexer *lx) {
  size_t start = lx->pos;
  int line = lx->line;
  int col = lx->col;
  char *bytes = arena_alloc(lx->arena, TEXT_BYTES + 1u);
  if (bytes == NULL) return diag_fail(lx->diag, "OOM", NULL, "%s: no memory for a Text literal", lx->source);
  size_t count = 0;
  advance(lx, 1);
  for (;;) {
    unsigned char c = (unsigned char)peek(lx, 0);
    if (lx->pos >= lx->len || c == '\n') {
      return diag_fail(lx->diag, "TEXT_CHAR", NULL, "%s:%d:%d: a Text literal has no closing quote", lx->source, line, col);
    }
    if (c == '"') break;
    size_t width = text_unit(lx, c);
    if (width == 0) return 0;
    size_t skip = c == '\\' ? 1u : 0u;
    size_t take = width - skip;
    if (count + take > TEXT_BYTES) {
      return diag_fail(lx->diag, "TEXT_CHAR", NULL, "%s:%d:%d: a Text literal is longer than %u bytes", lx->source, line, col, TEXT_BYTES);
    }
    memcpy(bytes + count, lx->text + lx->pos + skip, take);
    count += take;
    advance(lx, width);
  }
  advance(lx, 1);
  Token *tok = push(lx, TOK_TEXT, start, line, col);
  tok->nat = count;
  tok->bytes = bytes;
  return 1;
}

static int lex_token(Lexer *lx, char c) {
  if (is_ident_start(c)) return lex_ident(lx);
  if (is_digit(c)) return lex_nat(lx);
  if (c == '"') return lex_text(lx);
  return lex_symbol(lx);
}

int lex_source(Arena *arena, const char *source, const char *text, size_t len, TokenList *out, Diag *diag) {
  /* Each token takes at least one byte, and one EOF token follows. */
  Token *tokens = arena_alloc(arena, (len + 1) * sizeof(Token));
  if (tokens == NULL) return diag_fail(diag, "OOM", NULL, "%s: no memory for the tokens", source);
  Lexer lx = {
    .text = text, .len = len, .pos = 0, .line = 1, .col = 1,
    .out = tokens, .count = 0, .diag = diag, .source = source, .arena = arena,
  };
  for (;;) {
    skip_trivia(&lx);
    if (lx.pos >= lx.len) break;
    if (!lex_token(&lx, peek(&lx, 0))) return 0;
  }
  push(&lx, TOK_EOF, lx.pos, lx.line, lx.col);
  out->items = tokens;
  out->count = lx.count;
  return 1;
}
