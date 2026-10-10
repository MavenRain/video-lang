/* The langc command line of the tcc-media kit (forked from tcc-wasm).
   Exit 0: ok. Exit 1: the program is refused. Exit 2: usage or IO. */
#include <stdio.h>
#include <string.h>

#include "front/check.h"
#include "front/front.h"
#include "json.h"
#include "media.h"

#define ARENA_LIMIT_BYTES ((size_t)1 << 30)

typedef enum {
  CMD_CHECK,
  CMD_EVAL,
  CMD_BUILD,
  CMD_FFMPEG
} Command;

typedef struct {
  Command command;
  const char *prog_path;
  const char *entry; /* BUILD: NULL for the JSON document */
  char **args;
  int arg_count;
  const char *out_path; /* NULL: stdout */
} Options;

static const struct {
  const char *word;
  Command command;
} COMMANDS[] = {
  {"check", CMD_CHECK},
  {"eval", CMD_EVAL},
  {"build", CMD_BUILD},
  {"ffmpeg", CMD_FFMPEG},
};

static int usage(FILE *err) {
  fputs("usage: langc check PROG\n"
        "       langc eval PROG NAME [ARGS...]\n"
        "       langc build PROG [-o OUT]\n"
        "       langc build PROG MAIN IN... -o OUT\n"
        "       langc ffmpeg PROG MAIN IN... -o OUT\n", err);
  return 2;
}

static int find_command(const char *word, Command *out) {
  for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
    if (strcmp(COMMANDS[i].word, word) == 0) {
      *out = COMMANDS[i].command;
      return 1;
    }
  }
  return 0;
}

/* PROG MAIN IN... -o OUT: the media verbs (video-lang O-1). */
static int parse_media_options(int argc, char **argv, Options *opt, Diag *diag) {
  if (argc < 6 || strcmp(argv[argc - 2], "-o") != 0)
    return diag_fail(diag, "USAGE", NULL, "%s takes PROG MAIN IN... -o OUT", argv[1]);
  opt->entry = argv[3];
  opt->args = argv + 4;
  opt->arg_count = argc - 6;
  opt->out_path = argv[argc - 1];
  return 1;
}

static int parse_build_options(int argc, char **argv, Options *opt, Diag *diag) {
  if (argc == 3) return 1;
  if (argc == 5 && strcmp(argv[3], "-o") == 0) {
    opt->out_path = argv[4];
    return 1;
  }
  if (argc >= 6) return parse_media_options(argc, argv, opt, diag);
  return diag_fail(diag, "USAGE", NULL, "build takes PROG and an optional -o OUT, or PROG MAIN IN... -o OUT");
}

static int parse_options(int argc, char **argv, Options *opt, Diag *diag) {
  memset(opt, 0, sizeof *opt);
  if (argc < 3 || !find_command(argv[1], &opt->command)) {
    return diag_fail(diag, "USAGE", NULL, "expected a command and a program path");
  }
  opt->prog_path = argv[2];
  switch (opt->command) {
    case CMD_CHECK:
      return argc == 3 ? 1 : diag_fail(diag, "USAGE", NULL, "too many arguments");
    case CMD_EVAL:
      if (argc < 4) return diag_fail(diag, "USAGE", NULL, "eval needs a definition name");
      opt->entry = argv[3];
      opt->args = argv + 4;
      opt->arg_count = argc - 4;
      return 1;
    case CMD_BUILD:
      return parse_build_options(argc, argv, opt, diag);
    case CMD_FFMPEG:
      return parse_media_options(argc, argv, opt, diag);
  }
  return 0;
}

static int read_source(Arena *arena, const char *path, char **out, size_t *len, Diag *diag) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) return diag_fail(diag, "IO", NULL, "cannot open %s", path);
  char *buf = arena_alloc(arena, SOURCE_MAX_BYTES + 1);
  size_t count = buf == NULL ? 0 : fread(buf, 1, SOURCE_MAX_BYTES + 1, file);
  int read_error = ferror(file);
  fclose(file);
  if (buf == NULL) return diag_fail(diag, "OOM", NULL, "no memory for %s", path);
  if (read_error) return diag_fail(diag, "IO", NULL, "cannot read %s", path);
  if (count > SOURCE_MAX_BYTES) return diag_fail(diag, "IO_SIZE", NULL, "%s is larger than %zu bytes", path, SOURCE_MAX_BYTES);
  *out = buf;
  *len = count;
  return 1;
}

/* Writes the document only after it is complete, so a failure leaves no file. */
static int write_output(const char *path, const char *text, size_t len, Diag *diag) {
  FILE *file = path == NULL ? stdout : fopen(path, "wb");
  if (file == NULL) return diag_fail(diag, "IO", NULL, "cannot open %s", path);
  size_t count = fwrite(text, 1, len, file);
  int closed = path == NULL ? fflush(file) : fclose(file);
  return count == len && closed == 0 ? 1 : diag_fail(diag, "IO", NULL, "cannot write %s", path == NULL ? "stdout" : path);
}

static int run(const Options *opt, Arena *arena, Diag *diag) {
  char *text = NULL;
  size_t len = 0;
  if (!read_source(arena, opt->prog_path, &text, &len, diag)) return 2;
  DeclList decls;
  if (!front_load(arena, opt->prog_path, text, len, &decls, diag)) return 1;
  Machine machine;
  if (!check_program(arena, &decls, &machine, diag)) return 1;
  switch (opt->command) {
    case CMD_CHECK:
      puts("ok");
      return 0;
    case CMD_EVAL:
      return eval_command(&machine, opt->entry, opt->args, opt->arg_count, stdout, stderr);
    case CMD_BUILD: {
      if (opt->entry != NULL)
        return media_build(&machine, opt->entry, opt->args, opt->arg_count, opt->out_path);
      const char *doc = NULL;
      size_t doc_len = 0;
      if (!json_document(&machine, &doc, &doc_len)) return 1;
      return write_output(opt->out_path, doc, doc_len, diag) ? 0 : 2;
    }
    case CMD_FFMPEG:
      return media_ffmpeg(&machine, opt->entry, opt->args, opt->arg_count, opt->out_path, stdout);
  }
  return 1;
}

int main(int argc, char **argv) {
  Diag diag;
  diag_init(&diag);
  Options opt;
  if (!parse_options(argc, argv, &opt, &diag)) {
    diag_print(&diag, stderr);
    return usage(stderr);
  }
  Arena arena;
  arena_init(&arena, ARENA_LIMIT_BYTES);
  int status = run(&opt, &arena, &diag);
  arena_release(&arena);
  if (status != 0) diag_print(&diag, stderr);
  return status;
}
