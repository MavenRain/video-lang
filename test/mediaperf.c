/* 1080p perf runner (M3 Step 1, `make perf`). For each clip of the cap ladder
   and each entry of test/media/perf.lang: OURS = `langc build`, REF = the argv
   that `langc ffmpeg` prints, run by /bin/sh -c (D-M3-5). R1: the framemd5
   lines of OURS and REF are equal for every stream, and OURS has the expected
   video frames. Peak memory = ru_maxrss from wait4, the field that
   `/usr/bin/time -l` prints (D-M3-2). The ladder (D-M3-4): s10 and s20 first;
   s60 only when the O-3 checks on (s10, s20) hold and the projection is below
   the cap, so no run can pass the cap. Exit 0 only when every check holds. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* D-M3-3: the USER's "4 GB", the lower reading. */
static const long long CAP = 4000000000LL;
/* D-M3-4: the O-3 slack (256 MiB) over the growth of the clip bytes. */
static const long long SLACK = 268435456LL;
/* ru_maxrss is in bytes on macOS and in KiB on Linux. */
#ifdef __APPLE__
static const long long RSS_UNIT = 1;
#else
static const long long RSS_UNIT = 1024;
#endif
static const long RATE = 25;
static const char *const FONT = "/System/Library/Fonts/Supplemental/Arial.ttf";
static const char *const PROGRAM = "test/media/perf.lang";

/* An entry of perf.lang and the first source frame it keeps. */
/* An entry of perf.lang, the first and the last source frame it keeps (-1:
   the last frame of the clip) and the shortest clip it runs on, in seconds.
   Only the entries from s10 go into the O-3 checks and the s60 projection. */
typedef struct { const char *name; long first; long last; int from; } Entry;
#define ENTRIES 3
static const Entry ENTRY[ENTRIES] = {{"whole", 0, -1, 10}, {"chain", 25, -1, 10}, {"deep", 1400, 1499, 60}};
/* A rung of the ladder: test/media/perf/sSECONDS.mp4 and the OURS peaks. */
typedef struct { int seconds; long long bytes; long long rss[ENTRIES]; } Clip;
typedef struct { int status; long long rss; double wall; } Run;
/* The scratch files, under $TMPDIR. */
typedef struct { char dir[1024], ours[1100], ref[1100], ours_md5[1100], ref_md5[1100], argv[1100]; } Paths;

/* Start ARGV with stdin from /dev/null, stdout to OUT and stderr to ERR (NULL
   keeps ours). wait4 gives the exit status and the peak RSS. */
static Run run(char *const argv[], const char *out, const char *err) {
  Run r = {-1, 0, 0.0};
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) return r;
  struct timespec start, end;
  pid_t pid;
  int ok = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0) == 0
    && posix_spawn_file_actions_addopen(&actions, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0644) == 0
    && (err == NULL || posix_spawn_file_actions_addopen(&actions, 2, err, O_WRONLY, 0) == 0)
    && clock_gettime(CLOCK_MONOTONIC, &start) == 0
    && posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ) == 0;
  posix_spawn_file_actions_destroy(&actions);
  if (!ok) return r;
  int status = 0;
  struct rusage usage;
  pid_t done;
  while ((done = wait4(pid, &status, 0, &usage)) < 0 && errno == EINTR) continue;
  if (done != pid || clock_gettime(CLOCK_MONOTONIC, &end) != 0) return r;
  r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  r.rss = (long long)usage.ru_maxrss * RSS_UNIT;
  r.wall = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
  return r;
}

/* The gate `fm` function (test/gate.sh): framemd5 of every stream. */
static int framemd5(const char *ffmpeg, const char *media, const char *md5) {
  char *argv[] = {(char *)ffmpeg, "-nostdin", "-loglevel", "error", "-i", (char *)media,
    "-map", "0", "-f", "framemd5", "-", NULL};
  return run(argv, md5, NULL).status == 0;
}

/* R1 of one run: the framemd5 files are equal line for line, and *VIDEO gets
   the count of "0," lines (stream 0 = the video stream). The first differing
   pair is printed for a Step 1b hand-off. */
static int same_frames(const Paths *p, long *video) {
  FILE *a = fopen(p->ours_md5, "r");
  FILE *b = fopen(p->ref_md5, "r");
  int same = a != NULL && b != NULL;
  int more = same;
  char x[512], y[512];
  while (more) {
    char *left = fgets(x, sizeof x, a);
    char *right = fgets(y, sizeof y, b);
    more = left != NULL && right != NULL && strcmp(x, y) == 0;
    if (more) *video += strncmp(x, "0,", 2) == 0;
    if (!more && (left != NULL || right != NULL)) {
      if (left != NULL) x[strcspn(x, "\n")] = '\0';
      if (right != NULL) y[strcspn(y, "\n")] = '\0';
      printf("  first difference: OURS %s | REF %s\n", left == NULL ? "(end)" : x, right == NULL ? "(end)" : y);
      same = 0;
    }
  }
  if (a != NULL) { if (ferror(a)) same = 0; fclose(a); }
  if (b != NULL) { if (ferror(b)) same = 0; fclose(b); }
  return same;
}

static int read_line(const char *path, char *line, size_t size) {
  FILE *f = fopen(path, "r");
  if (f == NULL) return 0;
  int ok = fgets(line, (int)size, f) != NULL;
  fclose(f);
  if (ok) line[strcspn(line, "\n")] = '\0';
  return ok && line[0] != '\0';
}

static int failed(const Clip *clip, int e, const char *what, int status) {
  printf("FAIL perf s%d %s: %s exit %d\n", clip->seconds, ENTRY[e].name, what, status);
  return 0;
}

/* One run of ENTRY E on CLIP: OURS, REF, R1 and the row. */
static int measure(const Paths *p, const char *ffmpeg, Clip *clip, int e) {
  char input[64];
  snprintf(input, sizeof input, "test/media/perf/s%d.mp4", clip->seconds);
  long last = ENTRY[e].last < 0 ? clip->seconds * RATE - 1 : ENTRY[e].last;
  long frames = last - ENTRY[e].first + 1;
  char *name = (char *)ENTRY[e].name;
  char *build[] = {"build/langc", "build", (char *)PROGRAM, name, input, (char *)FONT, "-o", (char *)p->ours, NULL};
  char *print[] = {"build/langc", "ffmpeg", (char *)PROGRAM, name, input, (char *)FONT, "-o", (char *)p->ref, NULL};
  remove(p->ours);
  remove(p->ref);
  Run ours = run(build, "/dev/null", NULL);
  if (ours.status != 0) return failed(clip, e, "OURS langc build", ours.status);
  Run argv = run(print, p->argv, NULL);
  char line[8192];
  if (argv.status != 0 || !read_line(p->argv, line, sizeof line)) return failed(clip, e, "langc ffmpeg", argv.status);
  char *sh[] = {"/bin/sh", "-c", line, NULL};
  Run ref = run(sh, "/dev/null", "/dev/null");
  if (ref.status != 0) return failed(clip, e, "REF /bin/sh -c", ref.status);
  if (!framemd5(ffmpeg, p->ours, p->ours_md5) || !framemd5(ffmpeg, p->ref, p->ref_md5))
    return failed(clip, e, "framemd5", -1);
  long video = 0;
  int same = same_frames(p, &video);
  int r1 = same && video == frames;
  printf("s%-3d %-6s %6ld %12lld %12lld %8.2f %8.2f %s\n", clip->seconds, name, video, ours.rss, ref.rss,
    ours.wall, ref.wall, r1 ? "equal" : "DIFFER");
  fflush(stdout);
  if (same && video != frames) printf("FAIL perf s%d %s: OURS has %ld video frames, want %ld\n", clip->seconds, name, video, frames);
  if (!same) printf("FAIL perf s%d %s: the framemd5 of OURS and REF differ (R1)\n", clip->seconds, name);
  if (ours.rss >= CAP) printf("FAIL perf s%d %s: OURS peak %lld is not below the cap %lld\n", clip->seconds, name, ours.rss, CAP);
  clip->rss[e] = ours.rss;
  remove(p->ours);
  remove(p->ref);
  remove(p->ours_md5);
  remove(p->ref_md5);
  return r1 && ours.rss < CAP;
}

static int rung(const Paths *p, const char *ffmpeg, Clip *clip) {
  int ok = 1;
  for (int e = 0; ok && e < ENTRIES; e++) ok = clip->seconds < ENTRY[e].from || measure(p, ffmpeg, clip, e);
  return ok;
}

/* O-3 (D-M3-4): the demuxed packets are held, so the OURS peak grows with the
   compressed bytes of the clip, not with its decoded frames. */
static int o3(const Clip *a, const Clip *b, int e) {
  long long grew = b->rss[e] - a->rss[e];
  long long bytes = b->bytes - a->bytes;
  int ok = grew <= bytes + SLACK;
  printf("%s O-3 (s%d, s%d) %s: peak grew %lld <= clip bytes grew %lld + %lld\n", ok ? "ok  " : "FAIL",
    a->seconds, b->seconds, ENTRY[e].name, grew, bytes, SLACK);
  return ok;
}

/* The s60 projection from (s10, s20): 40 s more = 4 times the s10 to s20 step. */
static int projection(const Clip *a, const Clip *b, int e) {
  long long projected = b->rss[e] + 4 * (b->rss[e] - a->rss[e]);
  int ok = projected < CAP;
  printf("%s P s60 %s: %lld < cap %lld\n", ok ? "ok  " : "FAIL", ENTRY[e].name, projected, CAP);
  return ok;
}

static void clean(const Paths *p) {
  remove(p->ours);
  remove(p->ref);
  remove(p->ours_md5);
  remove(p->ref_md5);
  remove(p->argv);
  rmdir(p->dir);
}

int main(void) {
  const char *ffmpeg = getenv("FFMPEG");
  if (ffmpeg == NULL) ffmpeg = "ffmpeg";
  /* The REF argv names `ffmpeg`: put the pin first on PATH (as mediareview). */
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
  Paths p;
  int dir_len = snprintf(p.dir, sizeof p.dir, "%s/langc-media-perf.XXXXXX", tmp);
  if (dir_len < 0 || (size_t)dir_len >= sizeof p.dir || mkdtemp(p.dir) == NULL) return 1;
  snprintf(p.ours, sizeof p.ours, "%s/ours.mp4", p.dir);
  snprintf(p.ref, sizeof p.ref, "%s/ref.mp4", p.dir);
  snprintf(p.ours_md5, sizeof p.ours_md5, "%s/ours.md5", p.dir);
  snprintf(p.ref_md5, sizeof p.ref_md5, "%s/ref.md5", p.dir);
  snprintf(p.argv, sizeof p.argv, "%s/argv.txt", p.dir);

  Clip clip[3] = {{10, 0, {0}}, {20, 0, {0}}, {60, 0, {0}}};
  int ok = 1;
  for (int c = 0; ok && c < 3; c++) {
    char input[64];
    struct stat st;
    snprintf(input, sizeof input, "test/media/perf/s%d.mp4", clip[c].seconds);
    ok = stat(input, &st) == 0 && st.st_size > 0;
    if (ok) clip[c].bytes = (long long)st.st_size;
    else printf("FAIL perf: %s is missing (make perf makes it)\n", input);
  }
  if (ok) printf("rung entry frames     OURS rss      REF rss OURS s   REF s    R1\n");
  ok = ok && rung(&p, ffmpeg, &clip[0]) && rung(&p, ffmpeg, &clip[1]);
  int ladder = ok;
  for (int e = 0; ok && e < ENTRIES; e++) {
    int held = ENTRY[e].from > 10 || o3(&clip[0], &clip[1], e);
    int under = ENTRY[e].from > 10 || projection(&clip[0], &clip[1], e);
    ladder = ladder && held && under;
  }
  if (ok && !ladder) printf("FAIL perf: the (s10, s20) checks fail, so s60 does not run\n");
  ok = ladder && rung(&p, ffmpeg, &clip[2]);
  for (int e = 0; ok && e < ENTRIES; e++) ok = ENTRY[e].from > 10 || o3(&clip[0], &clip[2], e);
  clean(&p);
  int runs = 0;
  for (int c = 0; c < 3; c++)
    for (int e = 0; e < ENTRIES; e++) runs += clip[c].seconds >= ENTRY[e].from;
  if (ok) printf("perf: %d runs, R1 equal on each, every peak below the cap %lld, the ladder holds\n", runs, CAP);
  return ok ? 0 : 1;
}
