/* Media IO and timestamp regressions, including encoder failure cleanup. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
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
    "def frame : Video -> Video := fun (v : Video) => pad (padSpec 400 300 40 30 1056816) v\n";
  const char *entries[] = {"small", "box", "frame"};
  const char *names[] = {"scale preserves container SAR", "crop preserves container SAR", "pad preserves container SAR"};
  Arena arena; Diag diag; Machine m; DeclList decls;
  arena_init(&arena, (size_t)64 << 20); diag_init(&diag);
  int loaded = front_load(&arena, "spatial-sar.lang", program, strlen(program), &decls, &diag)
    && check_program(&arena, &decls, &m, &diag);
  for (unsigned i = 0; i < 3; i++) {
    /* FFV1 leaves the decoder SAR unknown; Matroska supplies the display SAR. */
    char *fixture_argv[] = {(char *)ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      i == 0 ? "testsrc=size=319x239:rate=25:duration=2" : "testsrc2=size=320x240:rate=25:duration=2",
      "-vf", i == 0 ? "setsar=1" : "setsar=4/3", "-c:v", "ffv1", "-pix_fmt", "yuv420p", input, NULL};
    int ok = loaded && child(fixture_argv) == 0;
    FILE *command = ok ? tmpfile() : NULL;
    if (command == NULL) ok = 0;
    if (ok) {
      char *inputs[] = {input};
      diag_init(&diag);
      ok = media_ffmpeg(&m, entries[i], inputs, 1, reference, command) == 0;
      rewind(command);
      char line[8192];
      size_t n = fread(line, 1, sizeof line - 1u, command);
      line[n] = '\0';
      char *argv[] = {"sh", "-c", line, NULL};
      ok = ok && child(argv) == 0 && media_build(&m, entries[i], inputs, 1, ours) == 0
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
  Cut cut = {{0, src.frames - 1u}, 0, INT64_MAX, NULL};
  status = encode(&m, &src, &cut, saved);
  old = fopen(saved, "rb");
  char bytes[64] = {0};
  if (old != NULL) { fread(bytes, 1, sizeof bytes - 1u, old); fclose(old); }
  expect("failed encoding keeps existing output", status != 0 && strcmp(bytes, "keep the existing output") == 0);
  source_close(&src);
  arena_release(&arena);
  spatial_sar_cases(ffmpeg, dir);
  remove(input); remove(fractional); remove(saved); rmdir(dir);
  printf("media IO regressions: %u checked, %u failures\n", checked, failed);
  return failed != 0;
}
