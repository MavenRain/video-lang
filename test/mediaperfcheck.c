/* C regressions for the perf runner's frame comparison. */
#define main mediaperf_program
#include "mediaperf.c"
#undef main

static int write_md5(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (f == NULL) return 0;
  int ok = fputs(text, f) >= 0;
  return fclose(f) == 0 && ok;
}

int main(void) {
  Paths p = {0};
  const char *tmp = getenv("TMPDIR");
  if (tmp == NULL || tmp[0] == '\0') tmp = "/tmp";
  int n = snprintf(p.dir, sizeof p.dir, "%s/langc-perf-check.XXXXXX", tmp);
  if (n < 0 || (size_t)n >= sizeof p.dir || mkdtemp(p.dir) == NULL) return 1;
  snprintf(p.ours_md5, sizeof p.ours_md5, "%s/ours.md5", p.dir);
  snprintf(p.ref_md5, sizeof p.ref_md5, "%s/ref.md5", p.dir);
  const char *row = "0, 0, 0, 1, 1, hash\n";
  const struct { const char *name, *ours, *ref; int same; long frames; } cases[] = {
    {"equal", row, row, 1, 1},
    {"empty OURS", "", row, 0, 0},
    {"empty REF", row, "", 0, 0},
    {"different", row, "0, 0, 0, 1, 1, other\n", 0, 0},
    {"short REF", "0, 0, 0, 1, 1, hash\n0, 1, 1, 1, 1, hash\n", row, 0, 1},
    {"empty", "", "", 1, 0},
  };
  int ok = 1;
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    long frames = 0;
    int wrote = write_md5(p.ours_md5, cases[i].ours) && write_md5(p.ref_md5, cases[i].ref);
    int same = wrote ? same_frames(&p, &frames) : -1;
    int pass = same == cases[i].same && frames == cases[i].frames;
    printf("%s perf comparison: %s\n", pass ? "ok  " : "FAIL", cases[i].name);
    ok = ok && pass;
  }
  /* Reading a directory fails. It must not count as matching empty files. */
  Paths bad = {0};
  snprintf(bad.ours_md5, sizeof bad.ours_md5, "%s", p.dir);
  snprintf(bad.ref_md5, sizeof bad.ref_md5, "%s", p.dir);
  long frames = 0;
  int rejected = !same_frames(&bad, &frames);
  printf("%s perf comparison: read error\n", rejected ? "ok  " : "FAIL");
  clean(&p);
  return ok && rejected ? 0 : 1;
}
