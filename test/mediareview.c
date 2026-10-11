/* Media IO and timestamp regressions, including encoder failure cleanup. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libavutil/opt.h>
#include "front/check.h"
#include "front/front.h"
#include "../src/media.c"

static unsigned failed, checked;
static void expect(const char *name, int ok) {
  checked++;
  if (!ok) { fprintf(stderr, "FAIL media IO %s\n", name); failed++; }
}
static int child(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) _exit(126);
    close(fd);
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
static int fixture(const char *ffmpeg, const char *rate, const char *path) {
  char input[128];
  snprintf(input, sizeof input, "testsrc2=size=320x240:rate=%s:duration=2", rate);
  char *argv[] = {(char *)ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i", input,
    "-c:v", "libx264", "-pix_fmt", "yuv420p", (char *)path, NULL};
  return child(argv) == 0;
}
static int exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && st.st_size > 0;
}
static int same_file(const char *a, const char *b) {
  FILE *left = fopen(a, "rb");
  FILE *right = fopen(b, "rb");
  if (left == NULL || right == NULL) {
    if (left != NULL) fclose(left);
    if (right != NULL) fclose(right);
    return 0;
  }
  int x, y;
  do { x = fgetc(left); y = fgetc(right); } while (x == y && x != EOF);
  int ok = x == y && !ferror(left) && !ferror(right);
  fclose(left); fclose(right);
  return ok;
}
static int frame_hashes(const char *ffmpeg, const char *input, const char *output) {
  char *argv[] = {(char *)ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-i", (char *)input,
    "-map", "0", "-f", "framemd5", (char *)output, NULL};
  return child(argv) == 0;
}
static void text_escape_cases(const char *dir) {
  static const char *texts[] = {" leading", "trailing ", "  ", "", "  a:b,c'd\\e%f[g];h=i \"j\"  "};
  const char *font = "/System/Library/Fonts/Supplemental/Arial.ttf";
  char spaced_font[1024];
  snprintf(spaced_font, sizeof spaced_font, "%s/font.ttf \t\n", dir);
  int linked = symlink(font, spaced_font) == 0;
  expect("make font with trailing whitespace", linked);
  for (unsigned i = 0; i < sizeof texts / sizeof texts[0] + 1u; i++) {
    const char *text = i < sizeof texts / sizeof texts[0] ? texts[i] : "Hi";
    const char *path = i < sizeof texts / sizeof texts[0] ? font : spaced_font;
    char chain[FILTER_MAX];
    size_t n = 0;
    int ok = put(chain, &n, "drawtext=fontfile=") && put_escaped(chain, &n, path, strlen(path))
      && put(chain, &n, ":text=") && put_escaped(chain, &n, text, strlen(text))
      && put(chain, &n, ":expansion=none:fontsize=32");
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterInOut *ins = NULL, *outs = NULL;
    ok = ok && graph != NULL && avfilter_graph_parse2(graph, chain, &ins, &outs) >= 0;
    uint8_t *actual_text = NULL, *actual_font = NULL;
    ok = ok && graph->nb_filters == 1u
      && av_opt_get(graph->filters[0]->priv, "text", 0, &actual_text) >= 0
      && av_opt_get(graph->filters[0]->priv, "fontfile", 0, &actual_font) >= 0
      && actual_text != NULL && actual_font != NULL
      && strcmp((const char *)actual_text, text) == 0 && strcmp((const char *)actual_font, path) == 0;
    expect(i < sizeof texts / sizeof texts[0] ? "drawtext preserves text bytes" : "drawtext preserves font path bytes", ok);
    av_free(actual_text); av_free(actual_font);
    avfilter_inout_free(&ins); avfilter_inout_free(&outs);
    avfilter_graph_free(&graph);
  }
  if (linked) unlink(spaced_font);
}

static void spatial_sar_cases(const char *ffmpeg, const char *dir) {
  char input[1088], ours[1088], reference[1088], ours_md5[1088], reference_md5[1088];
  snprintf(input, sizeof input, "%s/sar.mkv", dir);
  snprintf(ours, sizeof ours, "%s/sar-ours.mp4", dir);
  snprintf(reference, sizeof reference, "%s/sar-reference.mp4", dir);
  snprintf(ours_md5, sizeof ours_md5, "%s/sar-ours.md5", dir);
  snprintf(reference_md5, sizeof reference_md5, "%s/sar-reference.md5", dir);
  const char *program =
    "def small : Video -> Video := fun (v : Video) => scale (res 160 120) (trim (interval 10 29) v)\n"
    "def box : Video -> Video := fun (v : Video) => crop (rect 40 20 160 120) v\n"
    "def frame : Video -> Video := fun (v : Video) => pad (padSpec 400 300 40 30 1056816) v\n"
    "def textFirst : Video -> Font -> Video := fun (v : Video) (f : Font) => burnText (style f 32 16777215 10 20) (cons (pair (interval 0 1) \"Hi\") nil) v\n"
    "def textLater : Video -> Font -> Video := fun (v : Video) (f : Font) => burnText (style f 32 16777215 10 20) (cons (pair (interval 5 9) \"Hi\") nil) v\n"
    "def textOutside : Video -> Font -> Video := fun (v : Video) (f : Font) => burnText (style f 32 16777215 10 20) (cons (pair (interval 100 110) \"Hi\") nil) v\n";
  const char *entries[] = {"small", "box", "frame", "textFirst", "textLater", "textOutside"};
  const char *names[] = {"scale preserves container SAR", "crop preserves container SAR", "pad preserves container SAR",
    "first-frame text preserves container SAR", "later text preserves container SAR", "out-of-domain text preserves container SAR"};
  Arena arena; Diag diag; Machine m; DeclList decls;
  arena_init(&arena, (size_t)64 << 20); diag_init(&diag);
  int loaded = front_load(&arena, "spatial-sar.lang", program, strlen(program), &decls, &diag)
    && check_program(&arena, &decls, &m, &diag);
  for (unsigned i = 0; i < sizeof entries / sizeof entries[0]; i++) {
    /* FFV1 leaves the decoder SAR unknown; Matroska supplies the display SAR. */
    char *fixture_argv[] = {(char *)ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      i == 0 ? "testsrc=size=319x239:rate=25:duration=2" : "testsrc2=size=320x240:rate=25:duration=2",
      "-vf", i == 0 ? "setsar=1" : "setsar=4/3", "-c:v", "ffv1", "-pix_fmt", "yuv420p", input, NULL};
    int ok = loaded && child(fixture_argv) == 0;
    FILE *command = ok ? tmpfile() : NULL;
    if (command == NULL) ok = 0;
    if (ok) {
      char *inputs[] = {input, "/System/Library/Fonts/Supplemental/Arial.ttf"};
      int input_count = i < 3u ? 1 : 2;
      diag_init(&diag);
      ok = media_ffmpeg(&m, entries[i], inputs, input_count, reference, command) == 0;
      rewind(command);
      char line[8192];
      size_t n = fread(line, 1, sizeof line - 1u, command);
      line[n] = '\0';
      char *argv[] = {"sh", "-c", line, NULL};
      ok = ok && child(argv) == 0 && media_build(&m, entries[i], inputs, input_count, ours) == 0
        && frame_hashes(ffmpeg, ours, ours_md5) && frame_hashes(ffmpeg, reference, reference_md5)
        && same_file(ours_md5, reference_md5);
    }
    if (command != NULL) fclose(command);
    expect(names[i], ok);
    remove(ours); remove(reference); remove(ours_md5); remove(reference_md5);
  }
  remove(input);
  arena_release(&arena);
}
/* Each op alone, the exact edges of crop and pad, one even step past them, and
   a chain that follows the frame size through scale and crop. */
static const char PLAN_PROGRAM[] =
  "def trimmed : Video -> Video := fun (v : Video) => trim (interval 10 29) v\n"
  "def half : Video -> Video := fun (v : Video) => scale (res 160 120) v\n"
  "def corner : Video -> Video := fun (v : Video) => crop (rect 160 120 160 120) v\n"
  "def wideCrop : Video -> Video := fun (v : Video) => crop (rect 160 0 162 120) v\n"
  "def tallCrop : Video -> Video := fun (v : Video) => crop (rect 0 120 160 122) v\n"
  "def farCrop : Video -> Video := fun (v : Video) => crop (rect 16384 0 2 2) v\n"
  "def border : Video -> Video := fun (v : Video) => pad (padSpec 400 300 80 60 255) v\n"
  "def wideShift : Video -> Video := fun (v : Video) => pad (padSpec 400 300 82 60 0) v\n"
  "def tallShift : Video -> Video := fun (v : Video) => pad (padSpec 400 300 80 62 0) v\n"
  "def narrowPad : Video -> Video := fun (v : Video) => pad (padSpec 318 240 0 0 0) v\n"
  "def follow : Video -> Video := fun (v : Video) =>"
  " pad (padSpec 164 120 4 0 16777215) (crop (rect 480 360 160 120) (scale (res 640 480) v))\n"
  "def stale : Video -> Video := fun (v : Video) =>"
  " pad (padSpec 164 120 6 0 0) (crop (rect 480 360 160 120) (scale (res 640 480) v))\n"
  "def early : Video -> Video := fun (v : Video) => scale (res 160 120) (trim (interval 10 29) v)\n"
  "def late : Video -> Video := fun (v : Video) => trim (interval 10 29) (crop (rect 0 0 160 120) v)\n";

/* An entry of PLAN_PROGRAM on the 320x240 fixture without audio: its -vf, -af
   and spatial chain, or CODE (not NULL) when the plan is refused. */
static const struct {
  const char *name;
  const char *entry;
  const char *code;
  const char *vf;
  const char *af;
  const char *sf;
} PLAN_ROWS[] = {
  {"trim has no spatial step", "trimmed", NULL,
   "trim=start_frame=10:end_frame=30,setpts=PTS-STARTPTS", "", ""},
  {"scale text", "half", NULL, "scale=160:120,setpts=PTS-STARTPTS", "asetpts=PTS-STARTPTS", "scale=160:120"},
  {"crop at the exact edge", "corner", NULL,
   "crop=160:120:160:120,setpts=PTS-STARTPTS", "asetpts=PTS-STARTPTS", "crop=160:120:160:120"},
  {"crop past the width", "wideCrop", "CROP_RANGE", NULL, NULL, NULL},
  {"crop past the height", "tallCrop", "CROP_RANGE", NULL, NULL, NULL},
  {"crop offset past the width", "farCrop", "CROP_RANGE", NULL, NULL, NULL},
  {"pad at the exact edge", "border", NULL,
   "pad=400:300:80:60:color=0x0000ff,setpts=PTS-STARTPTS", "asetpts=PTS-STARTPTS", "pad=400:300:80:60:color=0x0000ff"},
  {"pad past the width", "wideShift", "PAD_RANGE", NULL, NULL, NULL},
  {"pad past the height", "tallShift", "PAD_RANGE", NULL, NULL, NULL},
  {"pad narrower than the frame", "narrowPad", "PAD_RANGE", NULL, NULL, NULL},
  {"chain follows the frame size", "follow", NULL,
   "scale=640:480,crop=160:120:480:360,pad=164:120:4:0:color=0xffffff,setpts=PTS-STARTPTS", "asetpts=PTS-STARTPTS",
   "scale=640:480,crop=160:120:480:360,pad=164:120:4:0:color=0xffffff"},
  {"chain pad past the followed width", "stale", "PAD_RANGE", NULL, NULL, NULL},
  {"spatial step after a trim", "early", NULL,
   "trim=start_frame=10:end_frame=30,setpts=PTS-STARTPTS,scale=160:120", "", "scale=160:120"},
  {"trim after a spatial step", "late", NULL,
   "crop=160:120:0:0,trim=start_frame=10:end_frame=30,setpts=PTS-STARTPTS", "", "crop=160:120:0:0"},
};

static void plan_cases(const char *input) {
  static char vf[FILTER_MAX], af[FILTER_MAX], sf[FILTER_MAX];
  Arena arena; Diag diag; Machine m; DeclList decls;
  char *inputs[] = {(char *)input};
  arena_init(&arena, (size_t)64 << 20); diag_init(&diag);
  int loaded = front_load(&arena, "plan.lang", PLAN_PROGRAM, strlen(PLAN_PROGRAM), &decls, &diag)
    && check_program(&arena, &decls, &m, &diag);
  for (size_t i = 0; i < sizeof PLAN_ROWS / sizeof PLAN_ROWS[0]; i++) {
    Source src;
    Cut cut;
    int status = 0;
    diag_init(&diag);
    int ok = loaded && prepare(&m, PLAN_ROWS[i].entry, inputs, 1, &src, vf, af, sf, &cut, &status);
    expect(PLAN_ROWS[i].name, PLAN_ROWS[i].code == NULL
      ? ok && strcmp(vf, PLAN_ROWS[i].vf) == 0 && strcmp(af, PLAN_ROWS[i].af) == 0 && strcmp(sf, PLAN_ROWS[i].sf) == 0
      : loaded && !ok && status == 1 && diag.code != NULL && strcmp(diag.code, PLAN_ROWS[i].code) == 0);
    if (loaded)
      source_close(&src);
  }
  arena_release(&arena);
}
int main(void) {
  const char *ffmpeg = getenv("FFMPEG");
  if (ffmpeg == NULL) ffmpeg = "ffmpeg";
  const char *slash = strrchr(ffmpeg, '/');
  if (slash != NULL) {
    const char *old = getenv("PATH");
    char path[8192];
    int n = snprintf(path, sizeof path, "%.*s:%s", (int)(slash - ffmpeg), ffmpeg, old == NULL ? "" : old);
    if (n < 0 || (size_t)n >= sizeof path || setenv("PATH", path, 1) != 0) return 1;
  }
  /* $TMPDIR first: a sandbox can deny /tmp. */
  const char *tmp = getenv("TMPDIR");
  if (tmp == NULL || tmp[0] == '\0') tmp = "/tmp";
  char dir[1024];
  int dir_len = snprintf(dir, sizeof dir, "%s/langc-media-review.XXXXXX", tmp);
  if (dir_len < 0 || (size_t)dir_len >= sizeof dir || mkdtemp(dir) == NULL) return 1;
  char input[sizeof dir + 64], fractional[sizeof dir + 64], output[sizeof dir + 64], saved[sizeof dir + 64];
  snprintf(input, sizeof input, "%s/input ' space.mp4", dir);
  snprintf(fractional, sizeof fractional, "%s/fractional.mkv", dir);
  snprintf(output, sizeof output, "%s/output ' space.mp4", dir);
  snprintf(saved, sizeof saved, "%s/existing.mp4", dir);
  if (!fixture(ffmpeg, "25", input) || !fixture(ffmpeg, "30000/1001", fractional)) return 1;
  Arena arena;
  Diag diag;
  Machine m;
  DeclList decls;
  const char *program = "def main : Video -> Video := fun (v : Video) => trim (interval 10 29) v\n";
  arena_init(&arena, (size_t)64 << 20);
  diag_init(&diag);
  if (!front_load(&arena, "media-io.lang", program, strlen(program), &decls, &diag)
      || !check_program(&arena, &decls, &m, &diag)) return 1;
  char *inputs[] = {input};
  FILE *command = tmpfile();
  if (command == NULL) return 1;
  int status = media_ffmpeg(&m, "main", inputs, 1, output, command);
  rewind(command);
  char line[4096];
  size_t n = fread(line, 1, sizeof line - 1u, command);
  line[n] = '\0';
  fclose(command);
  char *argv[] = {"sh", "-c", line, NULL};
  expect("quoted filenames execute as one argument", status == 0 && child(argv) == 0 && exists(output));
  remove(output);

  diag_init(&diag);
  FILE *readonly = fopen(input, "rb");
  if (readonly == NULL) return 1;
  status = media_ffmpeg(&m, "main", inputs, 1, output, readonly);
  fclose(readonly);
  expect("command write error is IO exit 2", status == 2 && diag.code != NULL && strcmp(diag.code, "IO") == 0);

  diag_init(&diag);
  inputs[0] = fractional;
  status = media_build(&m, "main", inputs, 1, output);
  expect("rounded CFR timestamps are accepted", status == 0 && exists(output));
  remove(output);

  diag_init(&diag);
  Source src;
  memset(&src, 0, sizeof src);
  src.vs = src.as = -1;
  status = 2;
  if (!source_open(&m, input, &src, &status)) return 1;
  for (size_t i = 0; i < src.pkt_count; i++) {
    if (src.pkts[i]->stream_index == src.vs) {
      memset(src.pkts[i]->data, 0, (size_t)src.pkts[i]->size);
      break;
    }
  }
  FILE *old = fopen(saved, "wb");
  if (old == NULL) return 1;
  fputs("keep the existing output", old);
  if (fclose(old) != 0) return 1;
  Cut cut = {{0, src.frames - 1u}, 0, INT64_MAX, NULL, {{0, 0, 0, NULL}}, 0};
  status = encode(&m, &src, &cut, saved);
  old = fopen(saved, "rb");
  char bytes[64] = {0};
  if (old != NULL) { fread(bytes, 1, sizeof bytes - 1u, old); fclose(old); }
  expect("failed encoding keeps existing output", status != 0 && strcmp(bytes, "keep the existing output") == 0);
  source_close(&src);
  arena_release(&arena);
  spatial_sar_cases(ffmpeg, dir);
  text_escape_cases(dir);
  plan_cases(input);
  remove(input); remove(fractional); remove(saved); rmdir(dir);
  printf("media IO regressions: %u checked, %u failures\n", checked, failed);
  return failed != 0;
}
