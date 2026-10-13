/* video-lang M1: the media back end of langc. The entry is evaluated on one
   neutral input per Video parameter; its residual is a chain of trim, scale,
   crop and pad over one input. The input is demuxed to its packets plus the
   position index (O-3, D2). The build decodes, keeps the trimmed positions
   and their audio span (O-4), rebases the timestamps (O-2), sends the kept
   frames through the spatial steps (one libavfilter graph) and encodes like
   the ffmpeg CLI does for .mp4 (O-5). The ffmpeg verb prints the reference
   argv of the same chain. M2 adds Font parameters and burnText: each track
   entry is one drawtext, active on its window of positions. In process the
   live graph is rebuilt each time the set of active entries changes. M6 adds
   concat: the plan is a list of segments (input, steps); the steps after a
   concat are pushed into each segment (D-M6-5); the reference argv is the
   -filter_complex form with the concat filter; the encode runs the segments
   in order with one encoder pair and the timing continues at the seam by the
   rule of that filter (D-M6-2, D-M6-8). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/fifo.h>
#include <libavutil/pixdesc.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "media.h"

#define STEP_MAX 64u
#define SEG_MAX 64u
#define TEXT_MAX 64u /* the in-process mask is one uint64_t */
#define PARAM_MAX 64u
#define FILTER_MAX 65536u
#define PLANE_MAX 64
#define ULL(x) ((unsigned long long)(x))
/* The one segment API that test/mediareview.c binds to (it includes this
   file); langc itself does not call it. */
#if defined(__GNUC__) || defined(__TINYC__)
#define REVIEW_API __attribute__((unused))
#else
#define REVIEW_API
#endif

typedef struct {
  uint64_t lo;
  uint64_t hi;
} Span;

typedef enum { STEP_TRIM, STEP_SCALE, STEP_CROP, STEP_PAD, STEP_TEXT } StepKind;

/* One op of the residual with the numbers of its first argument, in
   constructor order: interval lo hi, res w h, rect x y w h, padSpec w h x y
   color, style font size color x y (font = the input index). A text step
   also holds its track entries [e0, e1) of the plan table. ID is the
   residual step this one is a copy of: a step above a concat is copied into
   each segment under it, and ID is its row of the shift table (D-M6-5). */
typedef struct {
  StepKind kind;
  uint64_t n[5];
  uint32_t e0;
  uint32_t e1;
  uint32_t id;
  int reset; /* this step reads the new ranks of a concat */
} Step;

typedef struct {
  uint64_t next;
  int started;
} PositionShift;

/* One track entry: the positions lo..hi and the Text bytes. */
typedef struct {
  uint64_t lo;
  uint64_t hi;
  const char *bytes;
  uint64_t len;
} TextEntry;

/* One segment of the plan (D-M6-5): input INPUT under the steps [S0, S1) of
   the plan table, innermost first. The first INNER steps sit below every
   concat and count the positions of the source (D2); the steps after them
   were pushed through a concat and count the positions of the result of
   that concat. ABOVE is the number of steps (the last of the run) outside
   the lowest concat that this segment shares with the segment before it. */
typedef struct {
  uint32_t input;
  uint32_t s0;
  uint32_t s1;
  uint32_t inner;
  uint32_t above;
} Segment;

/* The residual of the entry: the segments in order over one step table.
   FONTS has bit i when parameter i is a Font. IDS counts the residual steps. */
typedef struct {
  Segment segs[SEG_MAX];
  uint32_t seg_count;
  Step steps[STEP_MAX];
  uint32_t step_count;
  TextEntry entries[TEXT_MAX];
  uint32_t entry_count;
  uint64_t fonts;
  uint32_t ids;
} Plan;

/* One drawtext of the in-process graph: active on the absolute positions
   lo..hi, inserted at offset AT of the spatial chain (an offset past the
   end: after the last spatial step). TEXT has no enable option. */
typedef struct {
  uint64_t lo;
  uint64_t hi;
  size_t at;
  const char *text;
} Burn;

/* One demuxed input: its video and audio packets in file order, and the
   position index (VPTS[p] = the pts of the frame at position p). */
typedef struct {
  AVFormatContext *fmt;
  int vs;
  int as; /* -1: no audio stream */
  AVPacket **pkts;
  size_t pkt_count;
  size_t pkt_cap;
  int64_t *vpts;
  uint64_t frames;
  AVRational fr;
  int sample_rate;
  int64_t a0; /* the first audio sample, in 1/sample_rate */
} Source;

/* The kept positions, the kept decoded audio samples [astart, aend), the
   spatial steps SF as one -vf chain (NULL or empty: no spatial step) and the
   drawtexts that go into it, in chain order (bit i of a mask = BURNS[i]). */
typedef struct {
  Span keep;
  int64_t astart;
  int64_t aend;
  const char *sf;
  Burn burns[TEXT_MAX];
  uint32_t burn_count;
} Cut;

/* The decode window of a cut (D-M5-1, D-M5-6): the video decode starts at
   packet FIRST in file order, and its first frame is position START. */
typedef struct {
  size_t first;
  uint64_t start;
} Window;

typedef struct {
  int64_t pts;
  int samples;
} AudioSpan;

typedef struct {
  AVFormatContext *oc;
  AVCodecContext *vdec;
  AVCodecContext *adec;
  AVCodecContext *venc;
  AVCodecContext *aenc;
  AVStream *vst;
  AVStream *ast;
  AVPacket *pkt;
  AVFrame *frame;
  AVFrame *chunk;
  AVAudioFifo *fifo;
  AVFifo *audio_times;
  AVFilterGraph *graph; /* NULL: the chain of MASK is empty */
  AVFilterContext *bsrc;
  AVFilterContext *bsink;
  AVFrame *filtered;
  AVRational fr;       /* the frame rate of the first segment: the encoder time base is 1/fr */
  int concat;          /* 2+ segments: the timing goes through the concat time base 1/1000000 */
  int64_t delta_us;    /* the start of the current segment on the concat clock */
  int64_t vend_us;     /* the video end of the current segment from its start (D-M6-2) */
  int64_t aend_us;     /* the audio end of the current segment from its start */
  int64_t last_pts;    /* the pts of the last video frame sent to the encoder */
  int collided;        /* a video frame rounds to the pts of the frame before it */
  uint64_t collide_at; /* the position of that frame */
  uint64_t position; /* the next video position out of the decoder */
  int skewed;        /* a decoded frame does not have the pts of its position */
  int64_t skew_pts;  /* the pts of that frame */
  int64_t seen;      /* audio samples out of the decoder */
  int64_t apts;      /* the next sample pts in the current audio span */
  int64_t aorigin;   /* the pts of the first kept audio sample */
  int64_t adelta;    /* sample clock for coarse container timestamps */
  int64_t anext;     /* the predicted pts of the next decoded audio frame */
  int64_t ashift;    /* the CLI input shift: minus the container start time */
  int astarted;
  int aremaining;
  uint64_t mask;     /* the burns of the live graph */
  const Source *src;
  Cut cut;
  Window window;
  char chain[FILTER_MAX];
} Enc;

static int find_entry(const Machine *m, const char *name, uint32_t *out) {
  for (uint32_t i = 0; i < m->def_count; i++) {
    if (m->defs[i].name != NULL && strcmp(m->defs[i].name, name) == 0) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

/* The ops of a plan by row: the step, the arity of the constructor of its
   first argument and the shape of that argument. */
static const struct {
  StepKind kind;
  uint32_t arity;
  const char *shape;
} STEP_ROWS[] = {
  {STEP_TRIM, 2u, "interval lo hi"},
  {STEP_SCALE, 2u, "res w h"},
  {STEP_CROP, 4u, "rect x y w h"},
  {STEP_PAD, 5u, "padSpec w h x y color"},
};

/* The row of V when V is a stuck trim, scale, crop or pad, 4 when V is a
   stuck burnText, else -1. */
static int step_row(const Value *v) {
  if (v->kind != VAL_STUCK)
    return -1;
  if (v->op == OP_BURN_TEXT)
    return v->argc == 3u ? 4 : -1;
  if (v->argc != 2u)
    return -1;
  return v->op == OP_TRIM ? 0 : v->op == OP_SCALE ? 1 : v->op == OP_CROP ? 2 : v->op == OP_PAD ? 3 : -1;
}

/* 1 when A is `style f size color x y` with f a Font parameter (one of
   FONTS) and numbers; they go to OUT in that order. */
static int style_of(const Value *a, uint64_t fonts, Step *out) {
  if (a->kind != VAL_OP || a->op != OP_MK_STYLE || a->argc != 5u)
    return 0;
  const Value *f = a->args[0];
  if (f->kind != VAL_VAR || f->nat >= PARAM_MAX || !(fonts >> f->nat & 1u))
    return 0;
  out->kind = STEP_TEXT;
  out->n[0] = f->nat;
  for (uint32_t i = 1; i < 5u; i++) {
    if (a->args[i]->kind != VAL_NAT)
      return 0;
    out->n[i] = a->args[i]->nat;
  }
  return 1;
}

/* Appends the entries of TRACK (a cons/nil list of `pair (interval lo hi)
   "..."`) to the plan table. Returns 0 (done), 1 (not that shape) or 2
   (more than TEXT_MAX entries). */
static int track_of(const Value *track, Plan *plan) {
  const Value *t = track;
  while (t->kind == VAL_OP && t->op == OP_CONS && t->argc == 2u) {
    const Value *p = t->args[0];
    if (p->kind != VAL_OP || p->op != OP_PAIR || p->argc != 2u)
      return 1;
    const Value *iv = p->args[0];
    const Value *text = p->args[1];
    if (iv->kind != VAL_OP || iv->op != OP_MK_INTERVAL || iv->argc != 2u || iv->args[0]->kind != VAL_NAT
        || iv->args[1]->kind != VAL_NAT || text->kind != VAL_TEXT)
      return 1;
    if (plan->entry_count == TEXT_MAX)
      return 2;
    plan->entries[plan->entry_count++] = (TextEntry){iv->args[0]->nat, iv->args[1]->nat, text->name, text->nat};
    t = t->args[1];
  }
  return t->kind == VAL_OP && t->op == OP_NIL ? 0 : 1;
}

/* 1 when A is the constructor of ROW with numeric fields; they go to OUT. */
static int step_of(const Value *a, int row, Step *out) {
  int ctor = row == 0 ? a->op == OP_MK_INTERVAL : row == 1 ? a->op == OP_MK_RES : row == 2 ? a->op == OP_MK_RECT : a->op == OP_MK_PAD_SPEC;
  if (a->kind != VAL_OP || !ctor || a->argc != STEP_ROWS[row].arity)
    return 0;
  out->kind = STEP_ROWS[row].kind;
  for (uint32_t i = 0; i < a->argc; i++) {
    if (a->args[i]->kind != VAL_NAT)
      return 0;
    out->n[i] = a->args[i]->nat;
  }
  return 1;
}

/* The walk of the residual (D-M6-5). STACK holds the steps that apply to the
   current node, the outermost at index 0. */
typedef struct {
  Machine *m;
  const char *entry;
  uint32_t level;
  Plan *plan;
  Step stack[STEP_MAX];
  uint32_t depth;
} Walk;

/* Puts the step V (of ROW) on the stack with a new id. */
static int peel(Walk *w, const Value *v, int row) {
  Plan *plan = w->plan;
  if (w->depth == STEP_MAX)
    return diag_fail(w->m->diag, "MEDIA_LIMIT", w->entry, "more than %u steps", STEP_MAX);
  Step *st = &w->stack[w->depth];
  if (row == 4) {
    if (!style_of(v->args[0], plan->fonts, st))
      return diag_fail(w->m->diag, "MEDIA_SHAPE", w->entry, "an argument is not `style f size color x y` with numbers");
    st->e0 = plan->entry_count;
    int track = track_of(v->args[1], plan);
    if (track == 2)
      return diag_fail(w->m->diag, "TEXT_LIMIT", w->entry, "more than %u track entries", TEXT_MAX);
    if (track != 0)
      return diag_fail(w->m->diag, "MEDIA_SHAPE", w->entry, "a track is not a list of `pair (interval lo hi) \"...\"`");
    st->e1 = plan->entry_count;
  } else if (!step_of(v->args[0], row, st)) {
    return diag_fail(w->m->diag, "MEDIA_SHAPE", w->entry, "an argument is not `%s` with numbers", STEP_ROWS[row].shape);
  }
  if (st->kind == STEP_TRIM && st->n[0] > st->n[1])
    return diag_fail(w->m->diag, "INTERVAL_ORDER", w->entry, "interval %llu %llu has lo > hi", ULL(st->n[0]), ULL(st->n[1]));
  st->id = plan->ids++;
  st->reset = 0;
  w->depth++;
  return 1;
}

/* One segment for the Video parameter V under the stack, innermost first.
   CONCAT is the stack depth at the lowest concat above V (0: none): the
   steps below it are inner. LCA is the stack depth at the concat where the
   segment before this one meets it (ABOVE of the segment). */
static int leaf(Walk *w, const Value *v, uint32_t concat, uint32_t lca) {
  Plan *plan = w->plan;
  if (plan->seg_count == SEG_MAX)
    return diag_fail(w->m->diag, "MEDIA_LIMIT", w->entry, "more than %u segments", SEG_MAX);
  if (w->depth > STEP_MAX - plan->step_count)
    return diag_fail(w->m->diag, "MEDIA_LIMIT", w->entry, "the segments hold more than %u steps", STEP_MAX);
  Segment *seg = &plan->segs[plan->seg_count++];
  seg->input = (uint32_t)v->nat;
  seg->s0 = plan->step_count;
  for (uint32_t k = 0; k < w->depth; k++)
    plan->steps[seg->s0 + k] = w->stack[w->depth - 1u - k];
  plan->step_count += w->depth;
  seg->s1 = plan->step_count;
  seg->inner = w->depth - concat;
  seg->above = lca;
  return 1;
}

/* The residual node V: its steps go on the stack, outermost first; then the
   core is a Video parameter (one segment) or a stuck concat (the segments
   of both operands, in order, D-M6-5). The steps come off on return. */
static int walk(Walk *w, const Value *v, uint32_t concat, uint32_t lca) {
  uint32_t from = w->depth;
  int ok = 1;
  int row = step_row(v);
  while (ok && row >= 0) {
    ok = peel(w, v, row);
    v = v->args[row == 4 ? 2 : 1];
    row = step_row(v);
  }
  if (ok && v->kind == VAL_STUCK && v->op == OP_CONCAT && v->argc == 2u) {
    if (w->depth > concat)
      w->stack[w->depth - 1u].reset = 1;
    ok = walk(w, v->args[0], w->depth, lca) && walk(w, v->args[1], w->depth, w->depth);
  } else if (ok && v->kind == VAL_VAR && v->nat < w->level && !(w->plan->fonts >> v->nat & 1u))
    ok = leaf(w, v, concat, lca);
  else if (ok)
    ok = diag_fail(w->m->diag, "MEDIA_SHAPE", w->entry,
                   "the result is not a chain of trim, scale, crop, pad, burnText and concat over inputs");
  w->depth = from;
  return ok;
}

/* Applies ENTRY to one neutral input per Video or Font parameter and reads
   the segments of the residual. *STATUS is the exit status of a failure. */
static int plan_of(Machine *m, const char *entry, int input_count, Plan *plan, int *status) {
  uint32_t d = 0;
  uint32_t level = 0;
  Walk w;
  *status = 1;
  m->def = entry;
  plan->entry_count = 0;
  plan->fonts = 0;
  plan->seg_count = 0;
  plan->step_count = 0;
  plan->ids = 0;
  if (!find_entry(m, entry, &d))
    return diag_fail(m->diag, "EVAL_ENTRY", entry, "no definition has this name");
  const Value *v = def_value(m, d);
  const Value *type = m->defs[d].type;
  while (v != NULL && type != NULL && type->kind == VAL_PI && (val_is(type->dom, OP_VIDEO) || val_is(type->dom, OP_FONT))) {
    if (level == PARAM_MAX)
      return diag_fail(m->diag, "MEDIA_LIMIT", entry, "more than %u parameters", PARAM_MAX);
    plan->fonts |= val_is(type->dom, OP_FONT) ? UINT64_C(1) << level : 0u;
    const Value *x = val_var(m, level);
    v = apply_value(m, v, x);
    type = type->body == NULL ? type->arg : closure_apply(m, type, x);
    level++;
  }
  if (v == NULL || type == NULL)
    return 0;
  if (!val_is(type, OP_VIDEO))
    return diag_fail(m->diag, "MEDIA_TYPE", entry, "the entry must have the type Video or Font -> ... -> Video");
  if (level != (uint32_t)input_count) {
    *status = 2;
    return diag_fail(m->diag, "MEDIA_ARGS", entry, "the entry takes %u inputs, found %d", level, input_count);
  }
  memset(&w, 0, sizeof w);
  w.m = m;
  w.entry = entry;
  w.level = level;
  w.plan = plan;
  return walk(&w, v, 0, 0);
}

static void source_close(Source *s) {
  for (size_t i = 0; i < s->pkt_count; i++)
    av_packet_free(&s->pkts[i]);
  av_free(s->pkts);
  av_free(s->vpts);
  avformat_close_input(&s->fmt);
}

static int cmp_pts(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a;
  int64_t y = *(const int64_t *)b;
  return (x > y) - (x < y);
}

static int keep_packet(Source *s, AVPacket *pkt) {
  if (s->pkt_count == s->pkt_cap) {
    size_t cap = s->pkt_cap == 0 ? 256u : s->pkt_cap * 2u;
    AVPacket **grown = av_realloc_array(s->pkts, cap, sizeof *grown);
    if (grown == NULL)
      return 0;
    s->pkts = grown;
    s->pkt_cap = cap;
  }
  AVPacket *copy = av_packet_alloc();
  if (copy == NULL)
    return 0;
  av_packet_move_ref(copy, pkt);
  s->pkts[s->pkt_count++] = copy;
  return 1;
}

/* The position index: the video pts in presentation order (D2). The frame
   rate can change in the stream (M4-R1), but each frame needs a pts, two
   frames never share one, and the video needs a frame rate for the encoder
   time base 1/fr (O-2). */
static int index_positions(Machine *m, const char *path, Source *s) {
  uint64_t k = 0;
  s->vpts = av_malloc_array((size_t)s->frames, sizeof *s->vpts);
  if (s->vpts == NULL)
    return diag_fail(m->diag, "OOM", NULL, "no memory for the index of %s", path);
  for (size_t i = 0; i < s->pkt_count; i++) {
    if (s->pkts[i]->stream_index == s->vs)
      s->vpts[k++] = s->pkts[i]->pts;
  }
  qsort(s->vpts, (size_t)s->frames, sizeof *s->vpts, cmp_pts);
  if (s->fr.num <= 0 || s->fr.den <= 0)
    return diag_fail(m->diag, "MEDIA_TIMESTAMPS", NULL, "%s: the video has no frame rate", path);
  if (s->vpts[0] == AV_NOPTS_VALUE)
    return diag_fail(m->diag, "MEDIA_TIMESTAMPS", NULL, "%s: a video frame has no pts", path);
  for (uint64_t p = 1; p < s->frames; p++) {
    if (s->vpts[p] <= s->vpts[p - 1u])
      return diag_fail(m->diag, "MEDIA_TIMESTAMPS", NULL, "%s: positions %llu and %llu have the same pts", path,
                       ULL(p - 1u), ULL(p));
  }
  return 1;
}

/* Demuxes PATH (O-3). *STATUS is the exit status of a failure. */
static int source_open(Machine *m, const char *path, Source *s, int *status) {
  *status = 2;
  if (avformat_open_input(&s->fmt, path, NULL, NULL) < 0)
    return diag_fail(m->diag, "MEDIA_INPUT", NULL, "cannot open %s", path);
  if (avformat_find_stream_info(s->fmt, NULL) < 0)
    return diag_fail(m->diag, "MEDIA_INPUT", NULL, "cannot read the streams of %s", path);
  *status = 1;
  s->vs = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (s->vs < 0)
    return diag_fail(m->diag, "MEDIA_NO_VIDEO", NULL, "%s has no video stream", path);
  int as = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  s->as = as < 0 ? -1 : as;
  AVStream *vst = s->fmt->streams[s->vs];
  s->fr = av_guess_frame_rate(s->fmt, vst, NULL);
  if (vst->codecpar->format != AV_PIX_FMT_YUV420P)
    return diag_fail(m->diag, "MEDIA_FORMAT", NULL, "%s: K0 takes yuv420p video only", path);
  if (s->as >= 0) {
    AVStream *ast = s->fmt->streams[s->as];
    s->sample_rate = ast->codecpar->sample_rate;
    if (s->sample_rate <= 0 || ast->codecpar->format != AV_SAMPLE_FMT_FLTP)
      return diag_fail(m->diag, "MEDIA_FORMAT", NULL, "%s: K0 takes fltp audio with a sample rate only", path);
    s->a0 = ast->start_time == AV_NOPTS_VALUE ? 0 : av_rescale_q(ast->start_time, ast->time_base, (AVRational){1, s->sample_rate});
  }
  AVPacket *pkt = av_packet_alloc();
  int ok = pkt != NULL;
  while (ok && av_read_frame(s->fmt, pkt) >= 0) {
    int video = pkt->stream_index == s->vs;
    s->frames += video ? 1u : 0u;
    ok = video || pkt->stream_index == s->as ? keep_packet(s, pkt) : 1;
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  if (!ok)
    return diag_fail(m->diag, "OOM", NULL, "no memory for the packets of %s", path);
  if (s->frames == 0)
    return diag_fail(m->diag, "EMPTY_VIDEO", NULL, "%s has no video frame", path);
  return index_positions(m, path, s);
}

/* The first decoded audio sample of the span of position P (O-4). */
static int64_t sample_at(const Source *s, uint64_t p) {
  AVRational tb = s->fmt->streams[s->vs]->time_base;
  int64_t x = av_rescale_q_rnd(s->vpts[p], tb, (AVRational){1, s->sample_rate}, AV_ROUND_DOWN) - s->a0;
  return x < 0 ? 0 : x;
}

/* PTS (time base TB) in the encoder time base 1/FR, rounded as fftools does
   with -fps_mode passthrough (adjust_frame_pts_to_encoder_tb): to the nearest
   tick, with extra bits and a small step away from an exact midpoint. */
static int64_t round_pts(int64_t pts, AVRational tb, AVRational fr) {
  AVRational enc = av_inv_q(fr);
  int extra = av_clip(29 - av_log2((unsigned)enc.den), 0, 16);
  AVRational fine = {enc.num, enc.den << extra};
  double x = (double)av_rescale_q(pts, tb, fine) / (double)(1 << extra);
  double step = x == (double)llrint(x) ? 0.0 : 1.0 / (1 << 17);
  return llrint(x + step);
}

/* The output pts of position P in a cut that starts at LO (O-2, M4-R2):
   vpts[P] - vpts[LO] in the encoder time base 1/fr of S. */
static int64_t out_pts(const Source *s, uint64_t lo, uint64_t p) {
  return round_pts(s->vpts[p] - s->vpts[lo], s->fmt->streams[s->vs]->time_base, s->fr);
}

/* vpts[P] - vpts[LO] in the concat time base 1/1000000: the concat filter
   rescales each input to it (D-M6-2). */
static int64_t rel_us(const Source *s, uint64_t lo, uint64_t p) {
  return av_rescale_q(s->vpts[p] - s->vpts[lo], s->fmt->streams[s->vs]->time_base, AV_TIME_BASE_Q);
}

/* The duration D of a decoded frame (stream time base) in the encoder time
   base 1/fr, as fftools sets it (video_sync_process). */
static int64_t out_duration(const Source *s, int64_t d) {
  return llrint((double)d * av_q2d(s->fmt->streams[s->vs]->time_base) / av_q2d(av_inv_q(s->fr)));
}

/* H.264 recovery points also have the key flag, but gradual intra refresh
   can require later pictures before decoding recovers. Only an IDR picture
   is an immediate start. Accept both length-prefixed AVC and Annex B NALs. */
static int decode_key(const Source *s, const AVPacket *pkt) {
  if ((pkt->flags & AV_PKT_FLAG_KEY) == 0)
    return 0;
  const AVCodecParameters *par = s->fmt->streams[s->vs]->codecpar;
  if (par->codec_id != AV_CODEC_ID_H264)
    return 1;
  const uint8_t *data = pkt->data;
  size_t size = (size_t)pkt->size;
  if (par->extradata_size >= 5 && par->extradata[0] == 1) {
    size_t length = (par->extradata[4] & 3u) + 1u;
    size_t at = 0;
    while (size - at >= length) {
      size_t n = 0;
      for (size_t j = 0; j < length; j++)
        n = (n << 8) | data[at++];
      if (n == 0 || n > size - at)
        return 0;
      if ((data[at] & 31u) == 5u)
        return 1;
      at += n;
    }
  } else {
    for (size_t i = 2; i + 1u < size; i++) {
      if (data[i - 2u] == 0 && data[i - 1u] == 0 && data[i] == 1 && (data[i + 1u] & 31u) == 5u)
        return 1;
    }
  }
  return 0;
}

/* The decode window of KEEP (D-M5-1, D-M5-6). It starts at the last decode
   key of a position at or before lo that splits presentation order: each
   earlier video packet has a lower pts and each later one a higher pts.
   Then its position is its video index. Without such a packet, the decode
   starts at packet 0 and position 0. */
static Window decode_window(const Source *s, Span keep) {
  int64_t after = INT64_MAX; /* the lowest pts of the video packets after packet i */
  uint64_t rank = s->frames; /* the video index of packet i */
  for (size_t i = s->pkt_count; i-- > 0;) {
    const AVPacket *pkt = s->pkts[i];
    int video = pkt->stream_index == s->vs;
    rank -= video ? 1u : 0u;
    if (video && rank <= keep.lo && s->vpts[rank] == pkt->pts && pkt->pts < after && decode_key(s, pkt))
      return (Window){i, rank};
    after = video && pkt->pts < after ? pkt->pts : after;
  }
  return (Window){0, 0};
}

static void drop_last_comma(char *text) {
  size_t len = strlen(text);
  if (len > 0 && text[len - 1u] == ',')
    text[len - 1u] = '\0';
}

/* Appends the formatted text to BUF (FILTER_MAX bytes) at *N; 0 when it
   does not fit. */
static int put(char *buf, size_t *n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf + *n, FILTER_MAX - *n, fmt, ap);
  va_end(ap);
  if (r < 0 || (size_t)r >= FILTER_MAX - *n)
    return 0;
  *n += (size_t)r;
  return 1;
}

/* The escape of the byte C (P16), or NULL when C stays as it is. Level 1
   (an option value) protects whitespace and puts `\` before `\`, `'` and
   `:`; then level 2 (the filtergraph) puts `\` before `\`, `'`, `[`, `]`,
   `,` and `;`. */
static const char *escape_byte(char c) {
  switch (c) {
  case '\\':
    return "\\\\\\\\";
  case '\'':
    return "\\\\\\'";
  case ':':
    return "\\\\:";
  case ' ':
    return "\\\\ ";
  case '\t':
    return "\\\\\t";
  case '\r':
    return "\\\\\r";
  case '\n':
    return "\\\\\n";
  case '[':
    return "\\[";
  case ']':
    return "\\]";
  case ',':
    return "\\,";
  case ';':
    return "\\;";
  }
  return NULL;
}

/* Appends the bytes S[0..LEN) to BUF at *N, escaped at both levels. */
static int put_escaped(char *buf, size_t *n, const char *s, uint64_t len) {
  int ok = 1;
  for (uint64_t i = 0; ok && i < len; i++) {
    const char *e = escape_byte(s[i]);
    ok = e == NULL ? put(buf, n, "%c", s[i]) : put(buf, n, "%s", e);
  }
  return ok;
}

/* MEDIA_FONT (B3): drawtext falls back to fontconfig on a fontfile it cannot
   load, so each Font input (bit i of FONTS) must be a face FreeType opens.
   *STATUS is the exit status of a failure. */
static int fonts_open(Machine *m, char *const *inputs, int input_count, uint64_t fonts, int *status) {
  FT_Library lib = NULL;
  int ok = 1;
  *status = 2;
  if (fonts == 0)
    return 1;
  if (FT_Init_FreeType(&lib) != 0)
    return diag_fail(m->diag, "MEDIA_FONT", NULL, "cannot start FreeType");
  for (int i = 0; ok && i < input_count; i++) {
    FT_Face face = NULL;
    if (fonts >> i & 1u) {
      ok = FT_New_Face(lib, inputs[i], 0, &face) == 0;
      if (ok)
        FT_Done_Face(face);
      else
        diag_fail(m->diag, "MEDIA_FONT", NULL, "%s is not a font FreeType can open", inputs[i]);
    }
  }
  FT_Done_FreeType(lib);
  return ok;
}

/* Restricts the position domain of S by each trim and follows the frame size
   through each spatial step, innermost first. VF and AF get the reference
   filter chains: each trim counts frames and samples relative to its own
   input (D2). SF gets the spatial steps alone. A text step puts one drawtext
   per entry in VF, with the window of the entry relative to the step input
   (an entry outside the domain is left out), and the same drawtext with no
   enable in OUT->burns, at its slot of SF. An empty domain (D3, D-M6-6), a
   crop outside its frame and a pad that does not hold its frame are refused.
   M6: the cut is of segment K of P over its source S. An inner step counts
   the positions of S (D2). A pushed step counts the positions of the result
   of the concat it came through: SHIFT[id].next is the input domain origin
   plus the frames of earlier segments at that step. Only a step directly
   above a concat resets the origin; later steps preserve it through trims.
   Each step adds its input frame count for the next segment (D-M6-4,
   D-M6-5). FIRST is the source of segment 0: with 2+ segments the pts round
   through 1/1000000 to its 1/fr (D-M6-2). */
static int cut_of(Machine *m, const char *entry, const Plan *p, uint32_t k, const Source *s, const Source *first, char *const *inputs, PositionShift *shift, char *vf, char *af, char *sf, Cut *out) {
  const Segment *seg = &p->segs[k];
  const AVCodecParameters *par = s->fmt->streams[s->vs]->codecpar;
  char draw[FILTER_MAX];
  int ok = 1;
  out->burn_count = 0;
  uint64_t dlo = 0;
  uint64_t dhi = s->frames - 1u;
  uint64_t domain_lo = 0;
  uint64_t w = (uint64_t)par->width;
  uint64_t h = (uint64_t)par->height;
  uint32_t trims = 0;
  size_t vn = 0;
  size_t an = 0;
  size_t sn = 0;
  int audio = s->as >= 0;
  vf[0] = '\0';
  af[0] = '\0';
  sf[0] = '\0';
  for (uint32_t i = seg->s0; i < seg->s1; i++) {
    const Step *st = &p->steps[i];
    const uint64_t *n = st->n;
    char text[160];
    int spatial = 1;
    int pushed = i - seg->s0 >= seg->inner;
    /* The positions of the step input as the step counts them (D-M6-5). */
    if (pushed && !shift[st->id].started) {
      shift[st->id].next = st->reset ? 0 : domain_lo;
      shift[st->id].started = 1;
    }
    uint64_t plo = pushed ? shift[st->id].next : dlo;
    uint64_t phi = plo + (dhi - dlo);
    domain_lo = plo;
    text[0] = '\0';
    switch (st->kind) {
    case STEP_TRIM: {
      if (n[1] < plo || n[0] > phi)
        return diag_fail(m->diag, "EMPTY_VIDEO", entry, "trim (interval %llu %llu) keeps no frame of positions %llu..%llu",
                         ULL(n[0]), ULL(n[1]), ULL(plo), ULL(phi));
      uint64_t lo = dlo + ((n[0] > plo ? n[0] : plo) - plo);
      uint64_t hi = dlo + ((n[1] < phi ? n[1] : phi) - plo);
      snprintf(text, sizeof text, "trim=start_frame=%llu:end_frame=%llu,setpts=PTS-STARTPTS,", ULL(lo - dlo), ULL(hi - dlo + 1u));
      if (audio) {
        int64_t base = sample_at(s, dlo);
        an += (size_t)snprintf(af + an, FILTER_MAX - an, "atrim=start_sample=%lld", (long long)(sample_at(s, lo) - base));
        if (hi + 1u < s->frames)
          an += (size_t)snprintf(af + an, FILTER_MAX - an, ":end_sample=%lld", (long long)(sample_at(s, hi + 1u) - base));
        an += (size_t)snprintf(af + an, FILTER_MAX - an, ",asetpts=PTS-STARTPTS,");
      }
      dlo = lo;
      dhi = hi;
      domain_lo = n[0] > plo ? n[0] : plo;
      trims++;
      spatial = 0;
      break;
    }
    case STEP_SCALE:
      snprintf(text, sizeof text, "scale=%llu:%llu,", ULL(n[0]), ULL(n[1]));
      w = n[0];
      h = n[1];
      break;
    case STEP_CROP:
      if (n[0] > w || n[2] > w - n[0] || n[1] > h || n[3] > h - n[1])
        return diag_fail(m->diag, "CROP_RANGE", entry, "rect %llu %llu %llu %llu is outside the %llux%llu frame",
                         ULL(n[0]), ULL(n[1]), ULL(n[2]), ULL(n[3]), ULL(w), ULL(h));
      snprintf(text, sizeof text, "crop=%llu:%llu:%llu:%llu,", ULL(n[2]), ULL(n[3]), ULL(n[0]), ULL(n[1]));
      w = n[2];
      h = n[3];
      break;
    case STEP_PAD:
      if (w > n[0] || n[2] > n[0] - w || h > n[1] || n[3] > n[1] - h)
        return diag_fail(m->diag, "PAD_RANGE", entry, "padSpec %llu %llu %llu %llu does not hold the %llux%llu frame",
                         ULL(n[0]), ULL(n[1]), ULL(n[2]), ULL(n[3]), ULL(w), ULL(h));
      snprintf(text, sizeof text, "pad=%llu:%llu:%llu:%llu:color=0x%06llx,", ULL(n[0]), ULL(n[1]), ULL(n[2]), ULL(n[3]), ULL(n[4]));
      w = n[0];
      h = n[1];
      break;
    case STEP_TEXT:
      for (uint32_t j = st->e0; ok && j < st->e1; j++) {
        const TextEntry *t = &p->entries[j];
        const char *fp = inputs[n[0]];
        size_t dn = 0;
        if (t->hi < plo || t->lo > phi)
          continue;
        uint64_t lo = dlo + ((t->lo > plo ? t->lo : plo) - plo);
        uint64_t hi = dlo + ((t->hi < phi ? t->hi : phi) - plo);
        ok = put(draw, &dn, "drawtext=fontfile=") && put_escaped(draw, &dn, fp, strlen(fp)) && put(draw, &dn, ":text=")
             && put_escaped(draw, &dn, t->bytes, t->len)
             && put(draw, &dn, ":expansion=none:fontsize=%llu:fontcolor=0x%06llx:x=%llu:y=%llu", ULL(n[1]), ULL(n[2]), ULL(n[3]), ULL(n[4]))
             && put(vf, &vn, "%s:enable='between(n,%llu,%llu)',", draw, ULL(lo - dlo), ULL(hi - dlo));
        const char *copy = ok ? arena_strndup(m->arena, draw, dn) : NULL;
        if (ok && copy == NULL)
          return diag_fail(m->diag, "OOM", NULL, "no memory for the text of %s", entry);
        if (ok)
          out->burns[out->burn_count++] = (Burn){lo, hi, sn, copy};
      }
      spatial = 0;
      break;
    }
    if (pushed)
      shift[st->id].next += phi - plo + 1u;
    ok = ok && put(vf, &vn, "%s", text) && (!spatial || put(sf, &sn, "%s", text));
    if (!ok)
      return diag_fail(m->diag, "MEDIA_LIMIT", entry, "a filter chain is longer than %u bytes", FILTER_MAX);
  }
  if (trims == 0) {
    if (!put(vf, &vn, "setpts=PTS-STARTPTS"))
      return diag_fail(m->diag, "MEDIA_LIMIT", entry, "a filter chain is longer than %u bytes", FILTER_MAX);
    snprintf(af, FILTER_MAX, "asetpts=PTS-STARTPTS");
  }
  drop_last_comma(vf);
  drop_last_comma(af);
  drop_last_comma(sf);
  /* Two kept frames at one output pts: the CLI warns and its muxer moves a
     dts. That output is not defined, so it is a refusal. With 2+ segments
     the pts goes through the concat time base to 1/fr of the first segment
     (D-M6-2); the seam offset can still move a rounding, which encode_all
     checks on the frame. */
  int concat = p->seg_count > 1u;
  for (uint64_t q = dlo + 1u; q <= dhi; q++) {
    int64_t a = concat ? round_pts(rel_us(s, dlo, q - 1u), AV_TIME_BASE_Q, first->fr) : out_pts(s, dlo, q - 1u);
    int64_t b = concat ? round_pts(rel_us(s, dlo, q), AV_TIME_BASE_Q, first->fr) : out_pts(s, dlo, q);
    if (b <= a)
      return diag_fail(m->diag, "MEDIA_TIMESTAMPS", entry, "positions %llu and %llu round to one pts at the frame rate %d/%d",
                       ULL(q - 1u), ULL(q), first->fr.num, first->fr.den);
  }
  out->keep.lo = dlo;
  out->keep.hi = dhi;
  out->astart = audio ? sample_at(s, dlo) : 0;
  out->aend = audio && dhi + 1u < s->frames ? sample_at(s, dhi + 1u) : INT64_MAX;
  out->sf = sf;
  return 1;
}

/* The frame size and SAR of segment K after COUNT steps of its run. Scale
   preserves display aspect ratio, while crop and pad preserve sample SAR. */
static void size_after(const Plan *p, uint32_t k, uint32_t count, const Source *s, uint64_t *w, uint64_t *h, AVRational *sar) {
  const AVCodecParameters *par = s->fmt->streams[s->vs]->codecpar;
  const Segment *seg = &p->segs[k];
  *w = (uint64_t)par->width;
  *h = (uint64_t)par->height;
  *sar = av_guess_sample_aspect_ratio(s->fmt, s->fmt->streams[s->vs], NULL);
  for (uint32_t i = seg->s0; i < seg->s0 + count; i++) {
    const uint64_t *n = p->steps[i].n;
    switch (p->steps[i].kind) {
    case STEP_SCALE:
      av_reduce(&sar->num, &sar->den, (int64_t)sar->num * (int64_t)*w * (int64_t)n[1],
                (int64_t)sar->den * (int64_t)*h * (int64_t)n[0], INT_MAX);
      *w = n[0];
      *h = n[1];
      break;
    case STEP_CROP:
      *w = n[2];
      *h = n[3];
      break;
    case STEP_PAD:
      *w = n[0];
      *h = n[1];
      break;
    case STEP_TRIM:
    case STEP_TEXT:
      break;
    }
  }
}

/* CONCAT_SHAPE (D-M6-3): the two operands of each concat must agree in frame
   size, pixel format, audio presence, sample rate and channel layout. The
   concat filter also needs matching known sample aspect ratios. Each
   pair of neighbour segments meets at the lowest concat above both (ABOVE
   of the second), where the frame size is the one after the steps below
   that concat; every concat is the meeting point of one such pair. The
   other properties come from the sources. */
static int shape_check(Machine *m, const char *entry, const Plan *p, const Source *srcs, char *const *inputs) {
  for (uint32_t k = 1; k < p->seg_count; k++) {
    const Segment *a = &p->segs[k - 1u];
    const Segment *b = &p->segs[k];
    const Source *sa = &srcs[k - 1u];
    const Source *sb = &srcs[k];
    const char *pa = inputs[a->input];
    const char *pb = inputs[b->input];
    const AVCodecParameters *va = sa->fmt->streams[sa->vs]->codecpar;
    const AVCodecParameters *vb = sb->fmt->streams[sb->vs]->codecpar;
    uint64_t wa = 0;
    uint64_t ha = 0;
    uint64_t wb = 0;
    uint64_t hb = 0;
    AVRational sara, sarb;
    size_after(p, k - 1u, (a->s1 - a->s0) - b->above, sa, &wa, &ha, &sara);
    size_after(p, k, (b->s1 - b->s0) - b->above, sb, &wb, &hb, &sarb);
    if (wa != wb || ha != hb)
      return diag_fail(m->diag, "CONCAT_SHAPE", entry, "concat: %s gives %llux%llu frames, %s gives %llux%llu",
                       pa, ULL(wa), ULL(ha), pb, ULL(wb), ULL(hb));
    if (sara.num > 0 && sarb.num > 0 && av_cmp_q(sara, sarb) != 0)
      return diag_fail(m->diag, "CONCAT_SHAPE", entry, "concat: %s gives SAR %d/%d, %s gives SAR %d/%d",
                       pa, sara.num, sara.den, pb, sarb.num, sarb.den);
    if (va->format != vb->format) {
      const char *fa = av_get_pix_fmt_name((enum AVPixelFormat)va->format);
      const char *fb = av_get_pix_fmt_name((enum AVPixelFormat)vb->format);
      return diag_fail(m->diag, "CONCAT_SHAPE", entry, "concat: %s is %s, %s is %s", pa, fa == NULL ? "unknown" : fa, pb,
                       fb == NULL ? "unknown" : fb);
    }
    if ((sa->as >= 0) != (sb->as >= 0))
      return diag_fail(m->diag, "CONCAT_SHAPE", entry, "concat: %s has %s, %s has %s", pa, sa->as >= 0 ? "audio" : "no audio", pb,
                       sb->as >= 0 ? "audio" : "no audio");
    if (sa->as >= 0) {
      const AVCodecParameters *aa = sa->fmt->streams[sa->as]->codecpar;
      const AVCodecParameters *ab = sb->fmt->streams[sb->as]->codecpar;
      char la[64];
      char lb[64];
      if (sa->sample_rate != sb->sample_rate)
        return diag_fail(m->diag, "CONCAT_SHAPE", entry, "concat: %s has %d Hz audio, %s has %d Hz", pa, sa->sample_rate, pb,
                         sb->sample_rate);
      if (av_channel_layout_compare(&aa->ch_layout, &ab->ch_layout) != 0) {
        if (av_channel_layout_describe(&aa->ch_layout, la, sizeof la) < 0)
          snprintf(la, sizeof la, "unknown");
        if (av_channel_layout_describe(&ab->ch_layout, lb, sizeof lb) < 0)
          snprintf(lb, sizeof lb, "unknown");
        return diag_fail(m->diag, "CONCAT_SHAPE", entry, "concat: %s has %s audio, %s has %s", pa, la, pb, lb);
      }
    }
  }
  return 1;
}

/* The plan with one demuxed source and one cut per segment (D-M6-7: the
   same file in two segments opens twice). OPENED counts the sources that
   source_open touched, for the close. */
typedef struct {
  Plan plan;
  Source srcs[SEG_MAX];
  Cut cuts[SEG_MAX];
  uint32_t opened;
} Build;

static void build_close(Build *b) {
  for (uint32_t k = 0; k < b->opened; k++)
    source_close(&b->srcs[k]);
}

/* Plan, demux and cut every segment, then the concat checks. VF, AF and SF
   are scratch (FILTER_MAX bytes each; after the call they hold the chains of
   the last segment); each cut keeps a copy of its SF in the arena. FC (not
   NULL: FILTER_MAX bytes) gets `[k:v]VF[vk];[k:a]AF[ak];` per segment, the
   input chains of the -filter_complex form (D-M6-5). *STATUS is the exit
   status of a failure. */
static int build_prepare(Machine *m, const char *entry, char *const *inputs, int input_count, Build *b, char *vf, char *af, char *sf, char *fc, int *status) {
  PositionShift shift[STEP_MAX] = {{0, 0}};
  size_t fn = 0;
  memset(b, 0, sizeof *b);
  av_log_set_level(AV_LOG_ERROR);
  if (!plan_of(m, entry, input_count, &b->plan, status) || !fonts_open(m, inputs, input_count, b->plan.fonts, status))
    return 0;
  for (uint32_t k = 0; k < b->plan.seg_count; k++) {
    b->srcs[k].vs = -1;
    b->srcs[k].as = -1;
    b->opened = k + 1u;
    if (!source_open(m, inputs[b->plan.segs[k].input], &b->srcs[k], status))
      return 0;
  }
  *status = 1;
  if (fc != NULL)
    fc[0] = '\0';
  for (uint32_t k = 0; k < b->plan.seg_count; k++) {
    Cut *cut = &b->cuts[k];
    if (!cut_of(m, entry, &b->plan, k, &b->srcs[k], &b->srcs[0], inputs, shift, vf, af, sf, cut))
      return 0;
    cut->sf = arena_strndup(m->arena, sf, strlen(sf));
    if (cut->sf == NULL)
      return diag_fail(m->diag, "OOM", NULL, "no memory for the chain of %s", entry);
    int audio = b->srcs[k].as >= 0;
    if (fc != NULL && !(put(fc, &fn, "[%u:v]%s[v%u];", k, vf, k) && (!audio || put(fc, &fn, "[%u:a]%s[a%u];", k, af, k))))
      return diag_fail(m->diag, "MEDIA_LIMIT", entry, "a filter chain is longer than %u bytes", FILTER_MAX);
  }
  /* The concat filter gives a one frame video segment the end 0 (D-M6-2),
     so without audio the next segment would start at the same pts. */
  for (uint32_t k = 0; k + 1u < b->plan.seg_count; k++) {
    if (b->cuts[k].keep.lo == b->cuts[k].keep.hi && b->srcs[k].as < 0)
      return diag_fail(m->diag, "MEDIA_TIMESTAMPS", entry, "segment %u keeps one frame and has no audio, so it has no length at the seam", k);
  }
  return shape_check(m, entry, &b->plan, b->srcs, inputs);
}

/* The one segment API: plan, demux and cut. A plan with a concat is refused
   here. *STATUS is the exit status of a failure, or the input index of the
   segment on success (the caller reads it there). */
REVIEW_API static int prepare(Machine *m, const char *entry, char *const *inputs, int input_count, Source *src, char *vf, char *af, char *sf, Cut *cut, int *status) {
  Build b;
  memset(src, 0, sizeof *src);
  src->vs = -1;
  src->as = -1;
  int ok = build_prepare(m, entry, inputs, input_count, &b, vf, af, sf, NULL, status);
  if (ok && b.plan.seg_count != 1u) {
    *status = 1;
    ok = diag_fail(m->diag, "MEDIA_SHAPE", entry, "this entry point takes one segment, the plan has %u", b.plan.seg_count);
  }
  if (!ok) {
    build_close(&b);
    return 0;
  }
  *src = b.srcs[0]; /* the caller closes it */
  *cut = b.cuts[0];
  *status = (int)b.plan.segs[0].input;
  return 1;
}

#define PATH_BARE "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./:-"

/* A shell word stays bare only when every byte is in BARE; else it is
   single-quoted, with `'` as `'\''`. */
static void shell_quote(FILE *out, const char *word, const char *bare) {
  size_t len = strlen(word);
  if (len != 0 && strspn(word, bare) == len) {
    fputs(word, out);
    return;
  }
  fputc('\'', out);
  for (const char *p = word; *p != '\0'; p++) {
    if (*p == '\'') fputs("'\\''", out);
    else fputc((unsigned char)*p, out);
  }
  fputc('\'', out);
}

static void shell_path(FILE *out, const char *path) {
  if (path[0] == '-') fputs("./", out);
  shell_quote(out, path, PATH_BARE);
}

/* A filter chain word: the bare set of a path plus `=` and `,`, so every M1
   chain stays bare. */
static void shell_word(FILE *out, const char *word) {
  shell_quote(out, word, PATH_BARE "=,");
}

/* One segment: `ffmpeg -i IN -vf VF [-af AF] -fps_mode passthrough OUT`.
   Two or more (D-M6-5): `ffmpeg -i IN0 -i IN1 ... -filter_complex FC -map
   [v] [-map [a]] -fps_mode passthrough OUT`, where FC holds the chain of
   each segment and `[v0][a0]...concat=n=K:v=1:a=1[v][a]` (a=0 and no audio
   labels without audio). */
int media_ffmpeg(Machine *m, const char *entry, char *const *inputs, int input_count, const char *out_path, FILE *out) {
  Build b;
  int status = 1;
  char vf[FILTER_MAX];
  char af[FILTER_MAX];
  char sf[FILTER_MAX];
  char fc[FILTER_MAX];
  int ok = build_prepare(m, entry, inputs, input_count, &b, vf, af, sf, fc, &status);
  uint32_t count = b.plan.seg_count;
  int audio = ok && b.srcs[0].as >= 0;
  size_t fn = ok ? strlen(fc) : 0;
  if (ok && count > 1u) {
    for (uint32_t k = 0; ok && k < count; k++)
      ok = audio ? put(fc, &fn, "[v%u][a%u]", k, k) : put(fc, &fn, "[v%u]", k);
    ok = ok && put(fc, &fn, "concat=n=%u:v=1:a=%d[v]%s", count, audio, audio ? "[a]" : "");
    if (!ok)
      diag_fail(m->diag, "MEDIA_LIMIT", entry, "a filter chain is longer than %u bytes", FILTER_MAX);
  }
  if (ok) {
    fputs("ffmpeg", out);
    for (uint32_t k = 0; k < count; k++) {
      fputs(" -i ", out);
      shell_path(out, inputs[b.plan.segs[k].input]);
    }
    if (count == 1u) {
      fputs(" -vf ", out);
      shell_word(out, vf);
      if (audio) {
        fputs(" -af ", out);
        shell_word(out, af);
      }
    } else {
      fputs(" -filter_complex ", out);
      shell_word(out, fc);
      fputs(" -map ", out);
      shell_word(out, "[v]");
      if (audio) {
        fputs(" -map ", out);
        shell_word(out, "[a]");
      }
      fprintf(out, " -enc_time_base:v %d:%d", b.srcs[0].fr.den, b.srcs[0].fr.num);
    }
    fputs(" -fps_mode passthrough ", out);
    shell_path(out, out_path);
    fputc('\n', out);
    if (ferror(out) || fflush(out) != 0) {
      diag_fail(m->diag, "IO", NULL, "cannot write the ffmpeg command");
      status = 2;
      ok = 0;
    }
  }
  build_close(&b);
  return ok ? 0 : status;
}

static AVCodecContext *open_decoder(const AVStream *st) {
  const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext *ctx = codec == NULL ? NULL : avcodec_alloc_context3(codec);
  AVDictionary *opts = NULL;
  int ok = ctx != NULL && avcodec_parameters_to_context(ctx, st->codecpar) >= 0;
  if (ok)
    ctx->pkt_timebase = st->time_base;
  ok = ok && av_dict_set(&opts, "threads", "auto", 0) >= 0 && avcodec_open2(ctx, codec, &opts) >= 0;
  av_dict_free(&opts);
  if (!ok)
    avcodec_free_context(&ctx);
  return ctx;
}

/* The encoder the ffmpeg CLI picks for PATH, set up like its encoder open
   (fftools enc_open): the frame properties (size, SAR, pixel format and color
   from the graph SINK when there is one, as fftools takes them from the
   filtered frame), the time base 1/fr (video) or 1/sample_rate (audio), the
   global header for mp4, threads=auto. */
static AVCodecContext *open_encoder(const AVFormatContext *oc, enum AVMediaType type, const char *path, const AVCodecContext *dec, const AVFilterContext *sink, AVRational fr) {
  const AVCodec *codec = avcodec_find_encoder(av_guess_codec(oc->oformat, NULL, path, NULL, type));
  AVCodecContext *enc = codec == NULL ? NULL : avcodec_alloc_context3(codec);
  AVDictionary *opts = NULL;
  if (enc == NULL)
    return NULL;
  int ok = 1;
  if (type == AVMEDIA_TYPE_VIDEO) {
    enc->width = sink == NULL ? dec->width : av_buffersink_get_w(sink);
    enc->height = sink == NULL ? dec->height : av_buffersink_get_h(sink);
    enc->pix_fmt = sink == NULL ? dec->pix_fmt : (enum AVPixelFormat)av_buffersink_get_format(sink);
    enc->sample_aspect_ratio = sink == NULL ? dec->sample_aspect_ratio : av_buffersink_get_sample_aspect_ratio(sink);
    enc->color_range = sink == NULL ? dec->color_range : av_buffersink_get_color_range(sink);
    enc->color_primaries = dec->color_primaries;
    enc->color_trc = dec->color_trc;
    enc->colorspace = sink == NULL ? dec->colorspace : av_buffersink_get_colorspace(sink);
    enc->chroma_sample_location = dec->chroma_sample_location;
    enc->time_base = av_inv_q(fr);
    enc->framerate = fr;
  } else {
    enc->sample_fmt = dec->sample_fmt;
    enc->sample_rate = dec->sample_rate;
    ok = av_channel_layout_copy(&enc->ch_layout, &dec->ch_layout) >= 0;
    enc->time_base = (AVRational){1, dec->sample_rate};
  }
  if (oc->oformat->flags & AVFMT_GLOBALHEADER)
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  enc->flags |= AV_CODEC_FLAG_FRAME_DURATION;
  ok = ok && av_dict_set(&opts, "threads", "auto", 0) >= 0 && avcodec_open2(enc, codec, &opts) >= 0;
  av_dict_free(&opts);
  if (!ok)
    avcodec_free_context(&enc);
  return enc;
}

static AVStream *add_stream(AVFormatContext *oc, const AVCodecContext *enc) {
  AVStream *st = avformat_new_stream(oc, NULL);
  if (st == NULL || avcodec_parameters_from_context(st->codecpar, enc) < 0)
    return NULL;
  st->time_base = enc->time_base;
  return st;
}

/* The spatial steps as one graph buffer -> SF -> buffersink, set up like the
   -vf graph of the ffmpeg CLI: the buffer takes the decoder frame properties
   at the time base 1/fr, and scale_sws_opts is flags=bicubic (the fftools sws
   default). Returns NULL, or the reason of the failure. */
static const char *graph_open(Enc *e, const Source *s, const char *chain) {
  const AVCodecContext *dec = e->vdec;
  AVFilterInOut *outs = avfilter_inout_alloc();
  AVFilterInOut *ins = avfilter_inout_alloc();
  AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
  e->graph = avfilter_graph_alloc();
  int ok = e->graph != NULL && outs != NULL && ins != NULL && par != NULL;
  if (ok) {
    par->format = dec->pix_fmt;
    par->time_base = av_inv_q(e->fr);
    par->frame_rate = e->fr;
    par->width = dec->width;
    par->height = dec->height;
    /* Container SAR can be known when the decoder has no SAR (e.g. FFV1). */
    par->sample_aspect_ratio = av_guess_sample_aspect_ratio(s->fmt, s->fmt->streams[s->vs], NULL);
    par->color_space = dec->colorspace;
    par->color_range = dec->color_range;
    e->graph->scale_sws_opts = av_strdup("flags=bicubic");
    e->bsrc = avfilter_graph_alloc_filter(e->graph, avfilter_get_by_name("buffer"), "in");
  }
  ok = ok && e->graph->scale_sws_opts != NULL && e->bsrc != NULL
       && av_buffersrc_parameters_set(e->bsrc, par) >= 0 && avfilter_init_dict(e->bsrc, NULL) >= 0
       && avfilter_graph_create_filter(&e->bsink, avfilter_get_by_name("buffersink"), "out", NULL, NULL, e->graph) >= 0;
  if (ok) {
    outs->name = av_strdup("in");
    outs->filter_ctx = e->bsrc;
    ins->name = av_strdup("out");
    ins->filter_ctx = e->bsink;
  }
  ok = ok && outs->name != NULL && ins->name != NULL
       && avfilter_graph_parse_ptr(e->graph, chain, &ins, &outs, NULL) >= 0
       && avfilter_graph_config(e->graph, NULL) >= 0;
  avfilter_inout_free(&ins);
  avfilter_inout_free(&outs);
  av_free(par);
  return ok ? NULL : "cannot set up the filter graph";
}

/* The bitset of the burns active at the absolute position P. */
static uint64_t mask_at(const Cut *c, uint64_t p) {
  uint64_t mask = 0;
  for (uint32_t i = 0; i < c->burn_count; i++)
    mask |= c->burns[i].lo <= p && p <= c->burns[i].hi ? UINT64_C(1) << i : 0u;
  return mask;
}

/* The spatial chain with the drawtext of each burn in MASK at its slot, into
   OUT (FILTER_MAX bytes). 0 when it does not fit. */
static int chain_of(const Cut *c, uint64_t mask, char *out) {
  const char *sf = c->sf == NULL ? "" : c->sf;
  size_t len = strlen(sf);
  size_t from = 0;
  size_t n = 0;
  int ok = 1;
  out[0] = '\0';
  for (uint32_t i = 0; ok && i < c->burn_count; i++) {
    if (!(mask >> i & 1u))
      continue;
    /* A slot past the end follows the last spatial step, whose comma SF
       dropped. */
    size_t at = c->burns[i].at < len ? c->burns[i].at : len;
    const char *comma = at > from && c->burns[i].at > len ? "," : "";
    ok = put(out, &n, "%.*s%s%s,", (int)(at - from), sf + from, comma, c->burns[i].text);
    from = at;
  }
  ok = ok && put(out, &n, "%s", sf + from);
  drop_last_comma(out);
  return ok;
}

/* Drains and frees the live graph, then sets up the graph of MASK (none when
   its chain is empty). The filters are pointwise and stateless, so a new
   graph does not change the pixels (P9-P14). */
static int filter_frame(Enc *e, AVFrame *f);
static int graph_switch(Enc *e, uint64_t mask) {
  int r = e->graph == NULL ? 0 : filter_frame(e, NULL);
  avfilter_graph_free(&e->graph);
  e->bsrc = NULL;
  e->bsink = NULL;
  e->mask = mask;
  if (r < 0)
    return r;
  if (!chain_of(&e->cut, mask, e->chain))
    return AVERROR(EINVAL);
  return e->chain[0] == '\0' || graph_open(e, e->src, e->chain) == NULL ? 0 : AVERROR(EINVAL);
}

/* The state of one segment (D-M6-7, D-M6-8): its source S and cut, its own
   decoders and decode window, a fresh audio clock, and its video end on the
   concat clock: the last kept pts times n/(n - 1) for n kept frames (the mean
   rate rule of the concat filter, P-M6-1), 0 for one frame. Returns NULL,
   or the reason of the failure. */
static const char *segment_open(Enc *e, const Source *s, const Cut *cut) {
  uint64_t n = cut->keep.hi - cut->keep.lo + 1u;
  avcodec_free_context(&e->vdec);
  avcodec_free_context(&e->adec);
  e->src = s;
  e->cut = *cut;
  e->window = decode_window(s, cut->keep);
  e->position = e->window.start;
  e->seen = 0;
  e->astarted = 0;
  e->adelta = AV_NOPTS_VALUE;
  e->anext = AV_NOPTS_VALUE;
  e->ashift = s->as < 0 || s->fmt->start_time == AV_NOPTS_VALUE ? 0
    : av_rescale_q(-s->fmt->start_time, AV_TIME_BASE_Q, s->fmt->streams[s->as]->time_base);
  e->vend_us = n >= 2u ? av_rescale(rel_us(s, cut->keep.lo, cut->keep.hi), (int64_t)n, (int64_t)n - 1) : 0;
  e->aend_us = 0;
  e->vdec = open_decoder(s->fmt->streams[s->vs]);
  e->adec = s->as < 0 ? NULL : open_decoder(s->fmt->streams[s->as]);
  if (e->vdec == NULL || (s->as >= 0 && e->adec == NULL))
    return "cannot open a decoder";
  /* Preserve container SAR when the first frame has no active text graph. */
  e->vdec->sample_aspect_ratio = av_guess_sample_aspect_ratio(s->fmt, s->fmt->streams[s->vs], NULL);
  return NULL;
}

/* Opens the first segment, its graph, the encoders, the muxer and the output
   file. Returns NULL, or the reason of the failure. */
static const char *enc_open(Enc *e, const Source *s, const Cut *cut, const char *path, const char *io_path) {
  if (avformat_alloc_output_context2(&e->oc, NULL, NULL, path) < 0 || e->oc == NULL)
    return "no muxer for the output name";
  e->fr = s->fr;
  e->last_pts = INT64_MIN;
  const char *why = segment_open(e, s, cut);
  if (why != NULL)
    return why;
  e->mask = mask_at(&e->cut, e->cut.keep.lo);
  if (!chain_of(&e->cut, e->mask, e->chain))
    return "a filter chain is too long";
  why = e->chain[0] == '\0' ? NULL : graph_open(e, s, e->chain);
  if (why != NULL)
    return why;
  e->venc = open_encoder(e->oc, AVMEDIA_TYPE_VIDEO, path, e->vdec, e->bsink, e->fr);
  e->aenc = e->adec == NULL ? NULL : open_encoder(e->oc, AVMEDIA_TYPE_AUDIO, path, e->adec, NULL, e->fr);
  if (e->venc == NULL || (e->adec != NULL && e->aenc == NULL))
    return "cannot open an encoder";
  e->vst = add_stream(e->oc, e->venc);
  e->ast = e->aenc == NULL ? NULL : add_stream(e->oc, e->aenc);
  if (e->vst == NULL || (e->aenc != NULL && e->ast == NULL))
    return "cannot add an output stream";
  e->vst->avg_frame_rate = e->fr;
  if (e->aenc != NULL) {
    int size = e->aenc->frame_size > 0 ? e->aenc->frame_size : 1024;
    e->fifo = av_audio_fifo_alloc(e->aenc->sample_fmt, e->aenc->ch_layout.nb_channels, size);
    e->audio_times = av_fifo_alloc2(8, sizeof(AudioSpan), AV_FIFO_FLAG_AUTO_GROW);
  }
  e->pkt = av_packet_alloc();
  e->frame = av_frame_alloc();
  e->chunk = av_frame_alloc();
  e->filtered = av_frame_alloc();
  if (e->pkt == NULL || e->frame == NULL || e->chunk == NULL || e->filtered == NULL
      || (e->aenc != NULL && (e->fifo == NULL || e->audio_times == NULL)))
    return "out of memory";
  if (avio_open(&e->oc->pb, io_path, AVIO_FLAG_WRITE) < 0)
    return "cannot open the output file";
  if (avformat_write_header(e->oc, NULL) < 0)
    return "cannot write the container header";
  return NULL;
}

static int enc_free(Enc *e) {
  int closed = e->oc == NULL ? 0 : avio_closep(&e->oc->pb);
  avformat_free_context(e->oc);
  avcodec_free_context(&e->vdec);
  avcodec_free_context(&e->adec);
  avcodec_free_context(&e->venc);
  avcodec_free_context(&e->aenc);
  av_audio_fifo_free(e->fifo);
  av_fifo_freep2(&e->audio_times);
  avfilter_graph_free(&e->graph);
  av_packet_free(&e->pkt);
  av_frame_free(&e->frame);
  av_frame_free(&e->chunk);
  av_frame_free(&e->filtered);
  return closed;
}

static int drained(int r) {
  return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : r;
}

/* Sends F (NULL: flush) to ENC and muxes every packet it gives. */
static int send(Enc *e, AVCodecContext *enc, const AVStream *st, const AVFrame *f) {
  int r = avcodec_send_frame(enc, f);
  while (r >= 0) {
    r = avcodec_receive_packet(enc, e->pkt);
    if (r >= 0) {
      av_packet_rescale_ts(e->pkt, enc->time_base, st->time_base);
      e->pkt->stream_index = st->index;
      r = av_interleaved_write_frame(e->oc, e->pkt);
    }
  }
  return drained(r);
}

/* Encodes F without the decoder picture type, which libx264 would force. F
   keeps the duration that video_frames set (the graph does not change it). */
static int video_out(Enc *e, AVFrame *f) {
  f->time_base = e->venc->time_base;
  f->pict_type = AV_PICTURE_TYPE_NONE;
  f->flags &= ~AV_FRAME_FLAG_KEY;
  return send(e, e->venc, e->vst, f);
}

/* Sends F (NULL: the end) through the graph and encodes every frame it
   gives. */
static int filter_frame(Enc *e, AVFrame *f) {
  int r = av_buffersrc_add_frame(e->bsrc, f);
  while (r >= 0) {
    r = av_buffersink_get_frame(e->bsink, e->filtered);
    if (r >= 0) {
      r = video_out(e, e->filtered);
      av_frame_unref(e->filtered);
    }
  }
  return drained(r);
}

/* The frame F out of the decoder must have the pts of the next position
   (D-M5-2). Each kept frame gets the source timing (O-2, M4-R2,
   setpts=PTS-STARTPTS and -fps_mode passthrough): its pts relative to the
   first kept frame and its own duration, both in the encoder time base. With
   2+ segments both go through the concat clock 1/1000000 and the pts starts
   at the seam (D-M6-2, D-M6-8); a pts at or under the last one is a
   collision. Then it goes through the graph of its active burns, if any. */
static int video_frame(Enc *e, AVFrame *f) {
  uint64_t p = e->position;
  if (f->pts != e->src->vpts[p]) {
    e->skewed = 1;
    e->skew_pts = f->pts;
    return AVERROR_INVALIDDATA;
  }
  e->position = p + 1u;
  if (p < e->cut.keep.lo)
    return 0;
  uint64_t mask = mask_at(&e->cut, p);
  if (e->concat) {
    AVRational tb = e->src->fmt->streams[e->src->vs]->time_base;
    f->pts = round_pts(rel_us(e->src, e->cut.keep.lo, p) + e->delta_us, AV_TIME_BASE_Q, e->fr);
    f->duration = llrint((double)av_rescale_q(f->duration, tb, AV_TIME_BASE_Q) * av_q2d(AV_TIME_BASE_Q) / av_q2d(av_inv_q(e->fr)));
  } else {
    f->pts = out_pts(e->src, e->cut.keep.lo, p);
    f->duration = out_duration(e->src, f->duration);
  }
  if (f->pts <= e->last_pts) {
    e->collided = 1;
    e->collide_at = p;
    return AVERROR_INVALIDDATA;
  }
  e->last_pts = f->pts;
  f->time_base = e->venc->time_base;
  int r = mask == e->mask ? 0 : graph_switch(e, mask);
  return r < 0 ? r : e->graph == NULL ? video_out(e, f) : filter_frame(e, f);
}

/* The frames out of the decoder after position hi are not checked and not
   kept. */
static int video_frames(Enc *e) {
  int r = 0;
  while (r >= 0 && (r = avcodec_receive_frame(e->vdec, e->frame)) >= 0) {
    r = e->position > e->cut.keep.hi ? 0 : video_frame(e, e->frame);
    av_frame_unref(e->frame);
  }
  return drained(r);
}

/* The pts of the first of N buffered samples. A chunk can cross a source
   timestamp gap; its pts follows its first sample, as the CLI buffersink
   does. Keep the next span's timing when a later chunk starts inside it. On
   the concat clock a partial take moves the span by its samples in
   1/1000000, as ff_framequeue_skip_samples does at each take. */
static int audio_pts(Enc *e, int n, int64_t *pts) {
  int first = 1;
  while (n > 0) {
    if (e->aremaining == 0) {
      AudioSpan span;
      if (av_fifo_read(e->audio_times, &span, 1) < 0)
        return AVERROR(EIO);
      e->apts = span.pts;
      e->aremaining = span.samples;
    }
    if (first) {
      *pts = e->apts;
      first = 0;
    }
    int take = n < e->aremaining ? n : e->aremaining;
    e->apts += e->concat ? av_rescale_q(take, e->aenc->time_base, AV_TIME_BASE_Q) : take;
    e->aremaining -= take;
    n -= take;
  }
  return 0;
}

/* One encoder frame of N samples from the FIFO (the CLI buffersink frame
   size: 1024 for aac, the last one short). */
static int audio_chunk(Enc *e, int n) {
  AVFrame *f = e->chunk;
  av_frame_unref(f);
  f->nb_samples = n;
  f->format = e->aenc->sample_fmt;
  f->sample_rate = e->aenc->sample_rate;
  if (av_channel_layout_copy(&f->ch_layout, &e->aenc->ch_layout) < 0 || av_frame_get_buffer(f, 0) < 0)
    return AVERROR(ENOMEM);
  if (av_audio_fifo_read(e->fifo, (void **)f->extended_data, n) != n)
    return AVERROR(EIO);
  int64_t pts = 0;
  if (audio_pts(e, n, &pts) < 0)
    return AVERROR(EIO);
  /* On the concat clock the pts comes back to 1/sample_rate as fftools
     rescales it for the encoder. */
  f->pts = e->concat ? av_rescale_q(pts, AV_TIME_BASE_Q, e->aenc->time_base) : pts;
  f->duration = n;
  f->time_base = e->aenc->time_base;
  return send(e, e->aenc, e->ast, f);
}

/* Encodes every full frame of the FIFO. */
static int audio_full(Enc *e) {
  int size = e->aenc->frame_size > 0 ? e->aenc->frame_size : 1024;
  int r = 0;
  while (r >= 0 && av_audio_fifo_size(e->fifo) >= size)
    r = audio_chunk(e, size);
  return r;
}

/* Keeps the samples of F inside [astart, aend) (counted from the first
   decoded sample, P7) and encodes every full frame. With 2+ segments the
   span goes on the concat clock: its pts in 1/1000000 plus the seam, and the
   audio end of the segment moves to its end (the concat filter push_frame). */
static int audio_keep(Enc *e, const AVFrame *f) {
  int64_t lo = e->seen;
  int64_t hi = e->seen + f->nb_samples;
  /* Match the CLI's audio clock (fftools audio_ts_process): keep real gaps,
     but do not turn rounded container timestamps into sample gaps (e.g. a
     Matroska 1 ms clock). The CLI shifts the input by the container start
     time, starts a new sample clock after a gap, and gives a frame without
     a pts the predicted pts. */
  AVRational tb = e->aenc->time_base;
  AVRational in = f->pts == AV_NOPTS_VALUE ? tb : e->src->fmt->streams[e->src->as]->time_base;
  int64_t ts = f->pts == AV_NOPTS_VALUE ? (e->anext == AV_NOPTS_VALUE ? 0 : e->anext) : f->pts + e->ashift;
  if (f->pts != AV_NOPTS_VALUE && e->anext != AV_NOPTS_VALUE && ts > av_rescale_q_rnd(e->anext, tb, in, AV_ROUND_UP))
    e->adelta = AV_NOPTS_VALUE;
  int64_t pts = av_rescale_delta(in, ts, tb, f->nb_samples, &e->adelta, tb);
  e->anext = pts + f->nb_samples;
  int64_t a = lo > e->cut.astart ? lo : e->cut.astart;
  int64_t b = hi < e->cut.aend ? hi : e->cut.aend;
  e->seen = hi;
  if (b <= a)
    return 0;
  int planar = av_sample_fmt_is_planar(f->format);
  int channels = f->ch_layout.nb_channels;
  int planes = planar ? channels : 1;
  size_t stride = (size_t)av_get_bytes_per_sample(f->format) * (size_t)(planar ? 1 : channels);
  uint8_t *ptrs[PLANE_MAX];
  if (planes > PLANE_MAX)
    return AVERROR(EINVAL);
  for (int c = 0; c < planes; c++)
    ptrs[c] = f->extended_data[c] + (size_t)(a - lo) * stride;
  if (av_audio_fifo_write(e->fifo, (void **)ptrs, (int)(b - a)) < (int)(b - a))
    return AVERROR(ENOMEM);
  pts += a - lo;
  if (!e->astarted) {
    e->aorigin = pts;
    e->astarted = 1;
  }
  AudioSpan span = {pts - e->aorigin, (int)(b - a)};
  if (e->concat) {
    int64_t rel = av_rescale_q(span.pts, tb, AV_TIME_BASE_Q);
    e->aend_us = rel + av_rescale_q(span.samples, tb, AV_TIME_BASE_Q);
    span.pts = rel + e->delta_us;
  }
  if (av_fifo_write(e->audio_times, &span, 1) < 0)
    return AVERROR(ENOMEM);
  return audio_full(e);
}

static int audio_frames(Enc *e) {
  int r = 0;
  while (r >= 0 && (r = avcodec_receive_frame(e->adec, e->frame)) >= 0) {
    r = audio_keep(e, e->frame);
    av_frame_unref(e->frame);
  }
  return drained(r);
}

/* N zero samples into the FIFO as one span at PTS (the concat clock), then
   every full frame. */
static int silence(Enc *e, int n, int64_t pts) {
  AVFrame *f = e->frame;
  av_frame_unref(f);
  f->nb_samples = n;
  f->format = e->aenc->sample_fmt;
  f->sample_rate = e->aenc->sample_rate;
  if (av_channel_layout_copy(&f->ch_layout, &e->aenc->ch_layout) < 0 || av_frame_get_buffer(f, 0) < 0)
    return AVERROR(ENOMEM);
  if (av_samples_set_silence(f->extended_data, 0, n, f->ch_layout.nb_channels, f->format) < 0)
    return AVERROR(EINVAL);
  int written = av_audio_fifo_write(e->fifo, (void **)f->extended_data, n);
  av_frame_unref(f);
  if (written < n)
    return AVERROR(ENOMEM);
  AudioSpan span = {pts, n};
  if (av_fifo_write(e->audio_times, &span, 1) < 0)
    return AVERROR(ENOMEM);
  return audio_full(e);
}

/* The end of a segment that another one follows (D-M6-2, the concat filter
   flush_segment): the seam is the maximum of the video end and the audio
   end of the segment on the concat clock; the audio gets silence up to the
   seam (send_silence: frames of max(9600, rate/5) zero samples, each with
   its own pts); the next segment starts at the seam. */
static int seam(Enc *e) {
  int64_t delta = e->vend_us;
  int r = 0;
  if (e->aenc != NULL) {
    AVRational rate = e->aenc->time_base;
    int frame = 9600 > e->aenc->sample_rate / 5 ? 9600 : e->aenc->sample_rate / 5;
    int64_t sent = 0;
    delta = e->aend_us > delta ? e->aend_us : delta;
    int64_t left = av_rescale_q(delta - e->aend_us, AV_TIME_BASE_Q, rate);
    while (r >= 0 && left > 0) {
      int n = left < frame ? (int)left : frame;
      r = silence(e, n, e->aend_us + e->delta_us + av_rescale_q(sent, rate, AV_TIME_BASE_Q));
      sent += n;
      left -= n;
    }
  }
  e->delta_us += delta;
  return r;
}

/* Decodes every audio packet and the video packets of the decode window of
   the current segment in file order, then flushes the decoders. The window
   sends no video packet after the decoder gives position hi (D-M5-1); the
   audio decode stays whole (D-M5-3). LAST also drains the graph, between
   the two flushes. */
static int enc_segment(Enc *e, const Source *s, int last) {
  int r = 0;
  for (size_t i = 0; r >= 0 && i < s->pkt_count; i++) {
    int video = s->pkts[i]->stream_index == s->vs;
    int skip = video && (i < e->window.first || e->position > e->cut.keep.hi);
    r = skip ? 0 : avcodec_send_packet(video ? e->vdec : e->adec, s->pkts[i]);
    if (r >= 0 && !skip)
      r = video ? video_frames(e) : audio_frames(e);
  }
  if (r >= 0 && (r = avcodec_send_packet(e->vdec, NULL)) >= 0)
    r = video_frames(e);
  if (r >= 0 && last && e->graph != NULL)
    r = filter_frame(e, NULL);
  if (r >= 0 && e->adec != NULL && (r = avcodec_send_packet(e->adec, NULL)) >= 0)
    r = audio_frames(e);
  return r;
}

/* The last short audio frame and the encoder flushes. */
static int enc_finish(Enc *e) {
  int r = e->aenc != NULL && av_audio_fifo_size(e->fifo) > 0 ? audio_chunk(e, av_audio_fifo_size(e->fifo)) : 0;
  if (r >= 0)
    r = send(e, e->venc, e->vst, NULL);
  if (r >= 0 && e->aenc != NULL)
    r = send(e, e->aenc, e->ast, NULL);
  return r;
}

/* Encodes COUNT segments (one source and one cut each) to PATH through a
   temporary name: one muxer and one encoder pair, the segments in order
   (D-M6-8), each with its own decoders and decode window, the seam of
   D-M6-2 between them. */
static int encode_all(Machine *m, const Source *srcs, const Cut *cuts, uint32_t count, const char *path) {
  size_t size = strlen(path) + sizeof ".XXXXXX";
  char *temporary = malloc(size);
  if (temporary == NULL)
    return diag_fail(m->diag, "OOM", NULL, "no memory for the output name") + 2;
  snprintf(temporary, size, "%s.XXXXXX", path);
  int fd = mkstemp(temporary);
  if (fd < 0) {
    free(temporary);
    return diag_fail(m->diag, "MEDIA_ENCODE", NULL, "%s: cannot open the output file", path) + 2;
  }
  if (close(fd) != 0) {
    remove(temporary);
    free(temporary);
    return diag_fail(m->diag, "MEDIA_ENCODE", NULL, "%s: cannot close the temporary file", path) + 2;
  }
  Enc e;
  memset(&e, 0, sizeof e);
  e.concat = count > 1u;
  const char *why = enc_open(&e, &srcs[0], &cuts[0], path, temporary);
  uint32_t k = 0;
  int r = 0;
  int ended = 0; /* the decode of segment k ended before its position hi */
  while (why == NULL && r >= 0 && !ended && k < count) {
    if (k > 0) {
      why = segment_open(&e, &srcs[k], &cuts[k]);
      r = why == NULL ? graph_switch(&e, mask_at(&e.cut, e.cut.keep.lo)) : 0;
    }
    if (why == NULL && r >= 0)
      r = enc_segment(&e, &srcs[k], k + 1u == count);
    /* The decode gave each position of [lo, hi], each with its pts (D-M5-2). */
    ended = why == NULL && r >= 0 && e.position <= e.cut.keep.hi;
    if (why == NULL && r >= 0 && !ended) {
      r = k + 1u < count ? seam(&e) : enc_finish(&e);
      k++;
    }
  }
  if (r < 0 && why == NULL && !e.skewed && !e.collided)
    why = "an encode step failed";
  int done = why == NULL && r >= 0 && !ended;
  if (done && av_write_trailer(e.oc) < 0)
    why = "cannot write the container trailer";
  const Source *s = &srcs[k < count ? k : 0];
  int skewed = e.skewed;
  int collided = e.collided;
  int64_t got = e.skew_pts;
  int64_t last = e.last_pts;
  uint64_t at = collided ? e.collide_at : e.position;
  uint64_t hi = e.cut.keep.hi;
  AVRational fr = e.fr;
  char where[40];
  where[0] = '\0';
  if (count > 1u)
    snprintf(where, sizeof where, "segment %u, ", k);
  if (enc_free(&e) < 0 && why == NULL)
    why = "cannot close the output file";
  if (why == NULL && done && rename(temporary, path) != 0)
    why = "cannot replace the output file";
  remove(temporary);
  free(temporary);
  if (why != NULL)
    return diag_fail(m->diag, "MEDIA_ENCODE", NULL, "%s: %s", path, why) + 2;
  if (skewed)
    return diag_fail(m->diag, "MEDIA_DECODE", NULL, "%sposition %llu: the decoded frame has pts %lld, the index has %lld",
                     where, ULL(at), (long long)got, (long long)s->vpts[at]) + 1;
  if (collided)
    return diag_fail(m->diag, "MEDIA_TIMESTAMPS", NULL, "%sposition %llu rounds to the pts %lld of the frame before it at the frame rate %d/%d",
                     where, ULL(at), (long long)last, fr.num, fr.den) + 1;
  if (!done)
    return diag_fail(m->diag, "MEDIA_DECODE", NULL, "%sthe decode ended before position %llu, the cut ends at %llu",
                     where, ULL(at), ULL(hi)) + 1;
  return 0;
}

/* The one segment API. */
REVIEW_API static int encode(Machine *m, const Source *s, const Cut *cut, const char *path) {
  return encode_all(m, s, cut, 1u, path);
}

int media_build(Machine *m, const char *entry, char *const *inputs, int input_count, const char *out_path) {
  Build b;
  int status = 1;
  char vf[FILTER_MAX];
  char af[FILTER_MAX];
  char sf[FILTER_MAX];
  int ok = build_prepare(m, entry, inputs, input_count, &b, vf, af, sf, NULL, &status);
  int result = ok ? encode_all(m, b.srcs, b.cuts, b.plan.seg_count, out_path) : status;
  build_close(&b);
  return result;
}
