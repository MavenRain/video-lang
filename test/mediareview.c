/* Media IO and timestamp regressions, including encoder failure cleanup. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libavutil/avstring.h>
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

/* Our build of ENTRY on INPUT and the reference command give equal framemd5
   lines for all streams (R1). */
static int equals_ffmpeg(Machine *m, const char *ffmpeg, const char *entry, const char *input, const char *dir) {
  char ours[1200], ref[1200], oh[1200], rh[1200];
  snprintf(ours, sizeof ours, "%s/vfr-ours.mp4", dir);
  snprintf(ref, sizeof ref, "%s/vfr-ref.mp4", dir);
  snprintf(oh, sizeof oh, "%s/vfr-ours.md5", dir);
  snprintf(rh, sizeof rh, "%s/vfr-ref.md5", dir);
  char *inputs[] = {(char *)input};
  FILE *command = tmpfile();
  int ok = command != NULL && media_build(m, entry, inputs, 1, ours) == 0
    && media_ffmpeg(m, entry, inputs, 1, ref, command) == 0;
  if (ok) {
    char line[8192];
    rewind(command);
    size_t n = fread(line, 1, sizeof line - 1u, command);
    line[n] = '\0';
    char *argv[] = {"sh", "-c", line, NULL};
    ok = !ferror(command) && n < sizeof line - 1u && child(argv) == 0
      && frame_hashes(ffmpeg, ours, oh) && frame_hashes(ffmpeg, ref, rh) && same_file(oh, rh);
  }
  if (command != NULL) fclose(command);
  remove(ours); remove(ref); remove(oh); remove(rh);
  return ok;
}

/* Seeking must preserve R1 for closed and open GOPs and gradual intra
   refresh. A recovery-point key packet may need later frames to recover. */
static void seek_cases(const char *ffmpeg, const char *dir) {
  Arena arena;
  Diag diag;
  Machine m;
  DeclList decls;
  const char *program =
    "def mid : Video -> Video := fun (v : Video) => trim (interval 50 69) v\n"
    "def tail : Video -> Video := fun (v : Video) => trim (interval 90 99) v\n";
  arena_init(&arena, (size_t)64 << 20);
  diag_init(&diag);
  int loaded = front_load(&arena, "seek.lang", program, strlen(program), &decls, &diag)
    && check_program(&arena, &decls, &m, &diag);
  expect("seek program loads", loaded);
  if (!loaded) { arena_release(&arena); return; }
  const char *kinds[] = {"closed", "open", "recovery"};
  const char *params[] = {
    "keyint=25:scenecut=0:bframes=3",
    "open-gop=1:keyint=25:scenecut=0:bframes=3",
    "intra-refresh=1:keyint=25:scenecut=0:bframes=0"
  };
  const char *containers[] = {"mp4", "mkv", "ts"};
  const char *entries[] = {"mid", "tail"};
  for (size_t container = 0; container < sizeof containers / sizeof containers[0]; container++) {
    for (size_t kind = 0; kind < sizeof kinds / sizeof kinds[0]; kind++) {
      char input[1200];
      snprintf(input, sizeof input, "%s/seek-%s.%s", dir, kinds[kind], containers[container]);
      char *args[] = {(char *)ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
        "testsrc2=size=320x240:rate=25:duration=4", "-c:v", "libx264", "-x264-params", (char *)params[kind],
        "-pix_fmt", "yuv420p", input, NULL};
      int made = child(args) == 0;
      for (size_t entry = 0; entry < sizeof entries / sizeof entries[0]; entry++) {
        diag_init(&diag);
        char name[160];
        snprintf(name, sizeof name, "seek %s %s %s equals ffmpeg", containers[container], kinds[kind], entries[entry]);
        expect(name, made && equals_ffmpeg(&m, ffmpeg, entries[entry], input, dir));
      }
      remove(input);
    }
  }
  arena_release(&arena);
}

/* VFR sources can have audio timestamp gaps. The sample cut is still
   atrim by sample count, but asetpts must keep each sample's timing.
   Matroska's 1 ms clock must not create gaps in continuous audio. */
static void vfr_audio_cases(const char *ffmpeg, const char *dir) {
  Arena arena;
  Diag diag;
  Machine m;
  DeclList decls;
  const char *program =
    "def whole : Video -> Video := fun (v : Video) => v\n"
    "def cut : Video -> Video := fun (v : Video) => trim (interval 10 25) v\n"
    "def nested : Video -> Video := fun (v : Video) => trim (interval 12 25) (trim (interval 10 29) v)\n"
    "def small : Video -> Video := fun (v : Video) => scale (res 80 60) (trim (interval 10 25) v)\n";
  const char *entries[] = {"whole", "cut", "nested", "small"};
  const char *containers[] = {"mp4", "mkv"};
  arena_init(&arena, (size_t)64 << 20);
  diag_init(&diag);
  int loaded = front_load(&arena, "vfr-audio.lang", program, strlen(program), &decls, &diag)
    && check_program(&arena, &decls, &m, &diag);
  char input[1200];
  for (unsigned gap = 0; gap < 2; gap++) {
    for (unsigned container = 0; container < 2; container++) {
      snprintf(input, sizeof input, "%s/vfr-audio.%s", dir, containers[container]);
      char *args[] = {(char *)ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
        "testsrc2=size=160x120:rate=25:duration=2", "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
        "-vf", "select='lt(n,15)+mod(n,2)'", "-af", gap ? "aselect='not(between(t,0.5,0.75))'" : "anull",
        "-fps_mode", "passthrough", "-enc_time_base:v", "1/90000", "-c:v", "libx264", "-pix_fmt", "yuv420p",
        "-c:a", "aac", input, NULL};
      int made = loaded && child(args) == 0;
      for (unsigned entry = 0; entry < sizeof entries / sizeof entries[0]; entry++) {
        diag_init(&diag);
        char name[128];
        snprintf(name, sizeof name, "VFR %s %s audio %s equals ffmpeg", containers[container],
                 gap ? "gapped" : "continuous", entries[entry]);
        expect(name, made && equals_ffmpeg(&m, ffmpeg, entries[entry], input, dir));
      }
      remove(input);
    }
  }
  arena_release(&arena);
}

/* The CLI audio clock on the Matroska 1 ms clock: a new sample clock after
   each timestamp gap (1 ms gaps here), and the input shift by the container
   start time (7 ms here) before the rounding to samples. */
static void audio_clock_cases(const char *ffmpeg, const char *dir) {
  Arena arena;
  Diag diag;
  Machine m;
  DeclList decls;
  const char *program =
    "def whole : Video -> Video := fun (v : Video) => v\n"
    "def cut : Video -> Video := fun (v : Video) => trim (interval 10 25) v\n"
    "def nested : Video -> Video := fun (v : Video) => trim (interval 12 25) (trim (interval 10 29) v)\n";
  const char *entries[] = {"whole", "cut", "nested"};
  const char *kinds[] = {"1 ms packet gaps", "start time 7 ms"};
  arena_init(&arena, (size_t)64 << 20);
  diag_init(&diag);
  int loaded = front_load(&arena, "audio-clock.lang", program, strlen(program), &decls, &diag)
    && check_program(&arena, &decls, &m, &diag);
  char base[1200], input[1200];
  snprintf(base, sizeof base, "%s/clock-base.mkv", dir);
  snprintf(input, sizeof input, "%s/clock.mkv", dir);
  char *make[] = {(char *)ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
    "testsrc2=size=160x120:rate=25:duration=2", "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
    "-af", "aselect='not(between(t,0.5,0.75))'", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", base, NULL};
  int based = loaded && child(make) == 0;
  for (unsigned kind = 0; kind < 2; kind++) {
    char *remux[] = {(char *)ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-i", base, "-c", "copy",
      kind ? "-output_ts_offset" : "-bsf:a", kind ? "0.007" : "setts=ts=TS+N", input, NULL};
    int made = based && child(remux) == 0;
    for (unsigned entry = 0; entry < sizeof entries / sizeof entries[0]; entry++) {
      diag_init(&diag);
      char name[128];
      snprintf(name, sizeof name, "mkv audio %s %s equals ffmpeg", kinds[kind], entries[entry]);
      expect(name, made && equals_ffmpeg(&m, ffmpeg, entries[entry], input, dir));
    }
    remove(input);
  }
  remove(base);
  arena_release(&arena);
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

/* The text of the one drawtext of CHAIN is WANT after the graph parses. */
static int parsed_text(const char *chain, const char *want) {
  AVFilterGraph *graph = avfilter_graph_alloc();
  AVFilterInOut *ins = NULL, *outs = NULL;
  uint8_t *text = NULL;
  int ok = graph != NULL && avfilter_graph_parse2(graph, chain, &ins, &outs) >= 0 && graph->nb_filters == 1u
    && av_opt_get(graph->filters[0]->priv, "text", 0, &text) >= 0 && text != NULL && strcmp((const char *)text, want) == 0;
  av_free(text);
  avfilter_inout_free(&ins); avfilter_inout_free(&outs);
  avfilter_graph_free(&graph);
  return ok;
}

/* Each byte that is syntax at level 1 (an option value: av_get_token on ":")
   or level 2 (the filtergraph: av_get_token on "[],;"), at both ends and in
   the middle of a text that sits between two options, as in the chain. Level
   2 must keep the whole option list, level 1 must give back the bytes up to
   the next option, and so must the parsed graph. Other bytes stay as they
   are. */
static const struct {
  char c;
  const char *escape;
} ESCAPES[] = {
  {'\\', "\\\\\\\\"}, {'\'', "\\\\\\'"}, {':', "\\\\:"}, {' ', "\\\\ "}, {'\t', "\\\\\t"}, {'\r', "\\\\\r"},
  {'\n', "\\\\\n"}, {'[', "\\["}, {']', "\\]"}, {',', "\\,"}, {';', "\\;"},
  {'a', NULL}, {'%', NULL}, {'=', NULL}, {'"', NULL}, {'{', NULL},
};

static void escaper_cases(void) {
  static char value[FILTER_MAX], chain[FILTER_MAX];
  for (size_t i = 0; i < sizeof ESCAPES / sizeof ESCAPES[0]; i++) {
    const char c = ESCAPES[i].c;
    const char text[4] = {c, 'a', c, '\0'};
    const char *e = escape_byte(c);
    size_t vn = 0, cn = 0;
    int ok = (e == NULL ? ESCAPES[i].escape == NULL : ESCAPES[i].escape != NULL && strcmp(e, ESCAPES[i].escape) == 0)
      && put(value, &vn, "t=") && put_escaped(value, &vn, text, 3u) && put(value, &vn, ":e");
    const char *p = value;
    char *level2 = ok ? av_get_token(&p, "[],;") : NULL;
    const char *q = level2 != NULL ? level2 + 2 : NULL;
    char *level1 = level2 != NULL && *p == '\0' && strncmp(level2, "t=", 2) == 0 ? av_get_token(&q, ":") : NULL;
    ok = ok && level1 != NULL && strcmp(q, ":e") == 0 && strcmp(level1, text) == 0
      && put(chain, &cn, "drawtext=fontfile=/System/Library/Fonts/Supplemental/Arial.ttf:text=")
      && put_escaped(chain, &cn, text, 3u) && put(chain, &cn, ":expansion=none") && parsed_text(chain, text);
    av_free(level2); av_free(level1);
    char name[64];
    snprintf(name, sizeof name, "escape of byte 0x%02x at both levels", (unsigned)(unsigned char)c);
    expect(name, ok);
  }
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

/* burnText windows on the 50-frame fixture: an entry is clamped to the
   positions of its step input (the enable counts from the input start, the
   burn keeps absolute positions) and left out when none of them is in it.
   A burn sits at its slot of the spatial chain. */
#define DRAW(t, lo, hi) \
  ":text=" t ":expansion=none:fontsize=32:fontcolor=0xffffff:x=10:y=20:enable='between(n," lo "," hi ")'"
#define BURN(name, track, v) \
  "def " name " : Video -> Font -> Video := fun (v : Video) (f : Font) => burnText (st f) " track " " v "\n"
#define ONE(lo, hi, t) "(cons (pair (interval " lo " " hi ") \"" t "\") nil)"
static const char TEXT_PROGRAM[] =
  "def st : Font -> Style := fun (f : Font) => style f 32 16777215 10 20\n"
  BURN("low", ONE("5", "14", "AA"), "(trim (interval 10 29) v)")
  BURN("high", ONE("25", "40", "AA"), "(trim (interval 10 29) v)")
  BURN("exact", ONE("10", "29", "AA"), "(trim (interval 10 29) v)")
  BURN("tail", ONE("45", "99", "AA"), "v")
  BURN("above", ONE("30", "45", "AA"), "(trim (interval 10 29) v)")
  BURN("below", ONE("0", "9", "AA"), "(trim (interval 10 29) v)")
  BURN("mixed", "(cons (pair (interval 0 9) \"AA\") " ONE("12", "15", "BB") ")", "(trim (interval 10 29) v)")
  "def slot : Video -> Font -> Video := fun (v : Video) (f : Font) =>"
  " pad (padSpec 200 160 20 20 0) (burnText (st f) " ONE("5", "9", "AA") " (scale (res 160 120) v))\n"
  BURN("last", ONE("5", "9", "AA"), "(scale (res 160 120) v)");

static const struct {
  const char *name;
  const char *entry;
  uint32_t count;
  uint64_t lo;
  uint64_t hi;
  size_t at;
  const char *draw;
  const char *sf;
} WINDOW_ROWS[] = {
  {"window clamped at the low end", "low", 1, 10, 14, 0, DRAW("AA", "0", "4"), ""},
  {"window clamped at the high end", "high", 1, 25, 29, 0, DRAW("AA", "15", "19"), ""},
  {"window on the exact domain", "exact", 1, 10, 29, 0, DRAW("AA", "0", "19"), ""},
  {"window clamped to the last frame", "tail", 1, 45, 49, 0, DRAW("AA", "45", "49"), ""},
  {"window one past the domain is left out", "above", 0, 0, 0, 0, NULL, ""},
  {"window one before the domain is left out", "below", 0, 0, 0, 0, NULL, ""},
  {"only the entry in the domain stays", "mixed", 1, 12, 15, 0, DRAW("BB", "2", "5"), ""},
  {"burn between two spatial steps", "slot", 1, 5, 9, 14, DRAW("AA", "5", "9"),
   "scale=160:120,pad=200:160:20:20:color=0x000000"},
  {"burn after the last spatial step", "last", 1, 5, 9, 14, DRAW("AA", "5", "9"), "scale=160:120"},
};

static unsigned count_of(const char *s, const char *word) {
  unsigned k = 0;
  for (const char *p = strstr(s, word); p != NULL; p = strstr(p + 1, word))
    k++;
  return k;
}

static void window_cases(const char *input) {
  static char vf[FILTER_MAX], af[FILTER_MAX], sf[FILTER_MAX];
  Arena arena; Diag diag; Machine m; DeclList decls;
  char *inputs[] = {(char *)input, "/System/Library/Fonts/Supplemental/Arial.ttf"};
  arena_init(&arena, (size_t)64 << 20); diag_init(&diag);
  int loaded = front_load(&arena, "text.lang", TEXT_PROGRAM, strlen(TEXT_PROGRAM), &decls, &diag)
    && check_program(&arena, &decls, &m, &diag);
  for (size_t i = 0; i < sizeof WINDOW_ROWS / sizeof WINDOW_ROWS[0]; i++) {
    Source src;
    Cut cut;
    int status = 0;
    diag_init(&diag);
    int ok = loaded && prepare(&m, WINDOW_ROWS[i].entry, inputs, 2, &src, vf, af, sf, &cut, &status)
      && cut.burn_count == WINDOW_ROWS[i].count && count_of(vf, "drawtext=") == WINDOW_ROWS[i].count
      && strcmp(sf, WINDOW_ROWS[i].sf) == 0;
    if (ok && WINDOW_ROWS[i].count != 0)
      ok = cut.burns[0].lo == WINDOW_ROWS[i].lo && cut.burns[0].hi == WINDOW_ROWS[i].hi && cut.burns[0].at == WINDOW_ROWS[i].at
        && strstr(vf, WINDOW_ROWS[i].draw) != NULL && strstr(cut.burns[0].text, "enable") == NULL;
    expect(WINDOW_ROWS[i].name, ok);
    if (loaded)
      source_close(&src);
  }
  arena_release(&arena);
}

/* Three burns over two spatial steps: A before the scale, B between the
   steps, C after the crop (a slot past the end of SF, whose last comma is
   dropped). A mask has bit i when burn i is active at a position. */
static void mask_cases(void) {
  static char out[FILTER_MAX];
  static const struct {
    uint64_t p;
    uint64_t mask;
  } AT[] = {{4, 0}, {5, 1}, {8, 3}, {9, 3}, {10, 2}, {12, 6}, {14, 6}, {15, 4}, {20, 4}, {21, 0}};
  static const struct {
    uint64_t mask;
    const char *chain;
  } CHAINS[] = {
    {0, "scale=160:120,crop=80:60:0:0"},
    {1, "A,scale=160:120,crop=80:60:0:0"},
    {2, "scale=160:120,B,crop=80:60:0:0"},
    {4, "scale=160:120,crop=80:60:0:0,C"},
    {7, "A,scale=160:120,B,crop=80:60:0:0,C"},
  };
  Cut c, bare;
  memset(&c, 0, sizeof c);
  memset(&bare, 0, sizeof bare);
  c.sf = "scale=160:120,crop=80:60:0:0";
  c.burns[0] = (Burn){5, 9, 0, "A"};
  c.burns[1] = (Burn){8, 14, 14, "B"};
  c.burns[2] = (Burn){12, 20, 29, "C"};
  c.burn_count = 3;
  int at_ok = 1;
  for (size_t i = 0; i < sizeof AT / sizeof AT[0]; i++)
    at_ok = at_ok && mask_at(&c, AT[i].p) == AT[i].mask;
  expect("mask_at sets the burns active at a position", at_ok);
  int chain_ok = 1;
  for (size_t i = 0; i < sizeof CHAINS / sizeof CHAINS[0]; i++)
    chain_ok = chain_ok && chain_of(&c, CHAINS[i].mask, out) && strcmp(out, CHAINS[i].chain) == 0;
  expect("chain_of puts each burn of the mask at its slot", chain_ok);
  bare.burns[0] = (Burn){0, 49, 0, "A"};
  bare.burns[1] = (Burn){0, 49, 0, "B"};
  bare.burn_count = 2;
  expect("chain_of with no spatial step", chain_of(&bare, 3, out) && strcmp(out, "A,B") == 0
    && chain_of(&bare, 0, out) && strcmp(out, "") == 0);
}

/* index_positions on COUNT video packets with the pts PTS at 25 fps: 1
   (accepted), 0 (refused, the code is in m->diag) or -1 (no memory). */
static int indexed(Machine *m, const int64_t *pts, size_t count) {
  AVPacket *pkts[4] = {NULL, NULL, NULL, NULL};
  Source s;
  memset(&s, 0, sizeof s);
  s.as = -1;
  s.fr = (AVRational){25, 1};
  s.pkts = pkts;
  s.pkt_count = count;
  s.frames = count;
  int ok = count <= 4u;
  for (size_t i = 0; ok && i < count; i++) {
    pkts[i] = av_packet_alloc();
    ok = pkts[i] != NULL;
    if (ok)
      pkts[i]->pts = pts[i];
  }
  diag_init(m->diag);
  ok = ok ? index_positions(m, "t.mp4", &s) : -1;
  av_freep(&s.vpts);
  for (size_t i = 0; i < count; i++)
    av_packet_free(&pkts[i]);
  return ok;
}

/* The M4 timing rules (O-2, M4-R1, M4-R2). out_pts: vpts[p] - vpts[lo] to
   the nearest tick of 1/fr, a tie up (fftools adds 1/2^17 off an integer).
   The tick at 25 fps is 3600 in 1/90000: 3564 is 0.99 tick, 9000 is 2.5 and
   12600 is 3.5. out_duration: a stream duration in ticks. index_positions
   accepts a VFR index and refuses a frame without a pts and two frames at
   one pts (no lavfi fixture makes these). */
static void timing_cases(Machine *m) {
  static const int64_t VFR[] = {0, 512, 1536};
  static const int64_t NOPTS[] = {0, AV_NOPTS_VALUE, 1024};
  static const int64_t SAME[] = {0, 512, 512};
  int64_t vpts[] = {0, 3564, 9000, 12600};
  AVFormatContext *fmt = avformat_alloc_context();
  AVStream *st = fmt == NULL ? NULL : avformat_new_stream(fmt, NULL);
  Source s;
  memset(&s, 0, sizeof s);
  s.fmt = fmt;
  s.as = -1;
  s.vpts = vpts;
  s.frames = 4;
  s.fr = (AVRational){25, 1};
  if (st != NULL)
    st->time_base = (AVRational){1, 90000};
  expect("out_pts rounds to the nearest tick of 1/fr, a tie up", st != NULL && out_pts(&s, 0, 0) == 0
    && out_pts(&s, 0, 1) == 1 && out_pts(&s, 0, 2) == 3 && out_pts(&s, 0, 3) == 4 && out_pts(&s, 1, 2) == 2
    && out_pts(&s, 1, 3) == 3);
  expect("out_duration is the stream duration in ticks of 1/fr", st != NULL && out_duration(&s, 3600) == 1
    && out_duration(&s, 7200) == 2);
  avformat_free_context(fmt);
  expect("a VFR index is accepted", indexed(m, VFR, 3) == 1);
  int nopts = indexed(m, NOPTS, 3) == 0 && m->diag->code != NULL && strcmp(m->diag->code, "MEDIA_TIMESTAMPS") == 0;
  expect("a frame without a pts is MEDIA_TIMESTAMPS", nopts);
  int same = indexed(m, SAME, 3) == 0 && m->diag->code != NULL && strcmp(m->diag->code, "MEDIA_TIMESTAMPS") == 0;
  expect("two frames at one pts is MEDIA_TIMESTAMPS", same);
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
  seek_cases(ffmpeg, dir);
  vfr_audio_cases(ffmpeg, dir);
  audio_clock_cases(ffmpeg, dir);
  text_escape_cases(dir);
  plan_cases(input);
  escaper_cases();
  window_cases(input);
  mask_cases();
  timing_cases(&m);
  remove(input); remove(fractional); remove(saved); rmdir(dir);
  printf("media IO regressions: %u checked, %u failures\n", checked, failed);
  return failed != 0;
}
