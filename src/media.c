/* video-lang K0: the media back end of langc. The entry is evaluated on one
   neutral input per Video parameter; its residual is a trim chain over one
   input. The input is demuxed to its packets plus the position index (O-3,
   D2). The build decodes, keeps the trimmed positions and their audio span
   (O-4), rebases the timestamps (O-2) and encodes like the ffmpeg CLI does for
   .mp4 (O-5). The ffmpeg verb prints the reference argv of the same cut. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>

#include "media.h"

#define TRIM_MAX 64u
#define FILTER_MAX 8192u
#define PLANE_MAX 64

typedef struct {
  uint64_t lo;
  uint64_t hi;
} Span;

/* The residual of the entry: input INPUT under TRIMS, innermost first. */
typedef struct {
  uint32_t input;
  Span trims[TRIM_MAX];
  uint32_t trim_count;
} Plan;

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

/* The kept positions and the kept decoded audio samples [astart, aend). */
typedef struct {
  Span keep;
  int64_t astart;
  int64_t aend;
} Cut;

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
  uint64_t position; /* the next video position out of the decoder */
  int64_t seen;      /* audio samples out of the decoder */
  int64_t apts;      /* audio samples sent to the encoder */
  Cut cut;
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

/* 1 when V is `interval lo hi` with numeric ends. */
static int interval_of(const Value *v, Span *out) {
  if (v->kind != VAL_OP || v->op != OP_MK_INTERVAL || v->argc != 2u)
    return 0;
  if (v->args[0]->kind != VAL_NAT || v->args[1]->kind != VAL_NAT)
    return 0;
  out->lo = v->args[0]->nat;
  out->hi = v->args[1]->nat;
  return 1;
}

/* Applies ENTRY to one neutral input per Video parameter and reads the trim
   chain of the residual. *STATUS is the exit status of a failure. */
static int plan_of(Machine *m, const char *entry, int input_count, Plan *plan, int *status) {
  uint32_t d = 0;
  uint32_t level = 0;
  Span rev[TRIM_MAX];
  uint32_t n = 0;
  *status = 1;
  m->def = entry;
  if (!find_entry(m, entry, &d))
    return diag_fail(m->diag, "EVAL_ENTRY", entry, "no definition has this name");
  const Value *v = def_value(m, d);
  const Value *type = m->defs[d].type;
  while (v != NULL && type != NULL && type->kind == VAL_PI && val_is(type->dom, OP_VIDEO)) {
    const Value *x = val_var(m, level);
    v = apply_value(m, v, x);
    type = type->body == NULL ? type->arg : closure_apply(m, type, x);
    level++;
  }
  if (v == NULL || type == NULL)
    return 0;
  if (!val_is(type, OP_VIDEO))
    return diag_fail(m->diag, "MEDIA_TYPE", entry, "the entry must have the type Video -> ... -> Video");
  if (level != (uint32_t)input_count) {
    *status = 2;
    return diag_fail(m->diag, "MEDIA_ARGS", entry, "the entry takes %u inputs, found %d", level, input_count);
  }
  while (v->kind == VAL_STUCK && v->op == OP_TRIM && v->argc == 2u) {
    if (n == TRIM_MAX)
      return diag_fail(m->diag, "MEDIA_LIMIT", entry, "more than %u trims", TRIM_MAX);
    if (!interval_of(v->args[0], &rev[n]))
      return diag_fail(m->diag, "MEDIA_SHAPE", entry, "a trim interval is not `interval lo hi` with numbers");
    if (rev[n].lo > rev[n].hi)
      return diag_fail(m->diag, "INTERVAL_ORDER", entry, "interval %llu %llu has lo > hi",
                       (unsigned long long)rev[n].lo, (unsigned long long)rev[n].hi);
    n++;
    v = v->args[1];
  }
  if (v->kind != VAL_VAR || v->nat >= level)
    return diag_fail(m->diag, "MEDIA_SHAPE", entry, "the result is not a trim chain over an input");
  plan->input = (uint32_t)v->nat;
  plan->trim_count = n;
  for (uint32_t k = 0; k < n; k++)
    plan->trims[k] = rev[n - 1u - k];
  return 1;
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

/* The position index: the video pts in presentation order (D2). A source is
   accepted only at a constant frame rate (O-2). */
static int index_positions(Machine *m, const char *path, Source *s) {
  AVStream *vst = s->fmt->streams[s->vs];
  uint64_t k = 0;
  s->vpts = av_malloc_array((size_t)s->frames, sizeof *s->vpts);
  if (s->vpts == NULL)
    return diag_fail(m->diag, "OOM", NULL, "no memory for the index of %s", path);
  for (size_t i = 0; i < s->pkt_count; i++) {
    if (s->pkts[i]->stream_index == s->vs)
      s->vpts[k++] = s->pkts[i]->pts;
  }
  qsort(s->vpts, (size_t)s->frames, sizeof *s->vpts, cmp_pts);
  if (s->fr.num <= 0 || s->fr.den <= 0 || s->vpts[0] == AV_NOPTS_VALUE)
    return diag_fail(m->diag, "VFR_SOURCE", NULL, "%s: the video has no constant frame rate", path);
  for (uint64_t p = 1; p < s->frames; p++) {
    /* CFR timestamps can round to alternating tick lengths. Compare each
       position with its rational offset, allowing one tick for rounding. */
    int64_t want = av_rescale_q((int64_t)p, av_inv_q(s->fr), vst->time_base);
    uint64_t delta = (uint64_t)s->vpts[p] - (uint64_t)s->vpts[0];
    if (s->vpts[p] <= s->vpts[p - 1u] || want <= 0
        || delta < (uint64_t)want - 1u || delta > (uint64_t)want + 1u)
      return diag_fail(m->diag, "VFR_SOURCE", NULL, "%s: position %llu is outside the constant frame rate", path,
                       (unsigned long long)p);
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

static void drop_last_comma(char *text) {
  size_t len = strlen(text);
  if (len > 0 && text[len - 1u] == ',')
    text[len - 1u] = '\0';
}

/* Restricts the position domain of S by each trim, innermost first. VF and
   AF get the reference filter chains: each trim counts frames and samples
   relative to its own input (D2). An empty domain is refused (D3). */
static int cut_of(Machine *m, const char *entry, const Plan *p, const Source *s, char *vf, char *af, Cut *out) {
  uint64_t dlo = 0;
  uint64_t dhi = s->frames - 1u;
  size_t vn = 0;
  size_t an = 0;
  int audio = s->as >= 0;
  vf[0] = '\0';
  af[0] = '\0';
  for (uint32_t k = 0; k < p->trim_count; k++) {
    uint64_t lo = p->trims[k].lo > dlo ? p->trims[k].lo : dlo;
    uint64_t hi = p->trims[k].hi < dhi ? p->trims[k].hi : dhi;
    if (lo > hi)
      return diag_fail(m->diag, "EMPTY_VIDEO", entry, "trim (interval %llu %llu) keeps no frame of positions %llu..%llu",
                       (unsigned long long)p->trims[k].lo, (unsigned long long)p->trims[k].hi, (unsigned long long)dlo, (unsigned long long)dhi);
    vn += (size_t)snprintf(vf + vn, FILTER_MAX - vn, "trim=start_frame=%llu:end_frame=%llu,setpts=PTS-STARTPTS,",
                           (unsigned long long)(lo - dlo), (unsigned long long)(hi - dlo + 1u));
    if (audio) {
      int64_t base = sample_at(s, dlo);
      an += (size_t)snprintf(af + an, FILTER_MAX - an, "atrim=start_sample=%lld", (long long)(sample_at(s, lo) - base));
      if (hi + 1u < s->frames)
        an += (size_t)snprintf(af + an, FILTER_MAX - an, ":end_sample=%lld", (long long)(sample_at(s, hi + 1u) - base));
      an += (size_t)snprintf(af + an, FILTER_MAX - an, ",asetpts=PTS-STARTPTS,");
    }
    dlo = lo;
    dhi = hi;
  }
  if (p->trim_count == 0) {
    snprintf(vf, FILTER_MAX, "setpts=PTS-STARTPTS");
    snprintf(af, FILTER_MAX, "asetpts=PTS-STARTPTS");
  }
  drop_last_comma(vf);
  drop_last_comma(af);
  out->keep.lo = dlo;
  out->keep.hi = dhi;
  out->astart = audio ? sample_at(s, dlo) : 0;
  out->aend = audio && dhi + 1u < s->frames ? sample_at(s, dhi + 1u) : INT64_MAX;
  return 1;
}

/* Plan, demux and cut. *STATUS is the exit status of a failure. */
static int prepare(Machine *m, const char *entry, char *const *inputs, int input_count, Source *src, char *vf, char *af, Cut *cut, int *status) {
  Plan plan;
  memset(src, 0, sizeof *src);
  src->vs = -1;
  src->as = -1;
  av_log_set_level(AV_LOG_ERROR);
  if (!plan_of(m, entry, input_count, &plan, status) || !source_open(m, inputs[plan.input], src, status))
    return 0;
  *status = 1;
  if (!cut_of(m, entry, &plan, src, vf, af, cut))
    return 0;
  *status = (int)plan.input; /* the caller reads the input index here */
  return 1;
}

/* A shell word stays bare only when every byte is safe in that position. */
static void shell_path(FILE *out, const char *path) {
  if (path[0] == '-') fputs("./", out);
  size_t len = strlen(path);
  if (len != 0 && strspn(path, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./:-") == len) {
    fputs(path, out);
    return;
  }
  fputc('\'', out);
  for (const char *p = path; *p != '\0'; p++) {
    if (*p == '\'') fputs("'\\''", out);
    else fputc((unsigned char)*p, out);
  }
  fputc('\'', out);
}

int media_ffmpeg(Machine *m, const char *entry, char *const *inputs, int input_count, const char *out_path, FILE *out) {
  Source src;
  Cut cut;
  int status = 1;
  char vf[FILTER_MAX];
  char af[FILTER_MAX];
  int ok = prepare(m, entry, inputs, input_count, &src, vf, af, &cut, &status);
  if (ok) {
    fputs("ffmpeg -i ", out);
    shell_path(out, inputs[status]);
    fprintf(out, " -vf %s", vf);
    if (src.as >= 0)
      fprintf(out, " -af %s", af);
    fputc(' ', out);
    shell_path(out, out_path);
    fputc('\n', out);
    if (ferror(out) || fflush(out) != 0) {
      diag_fail(m->diag, "IO", NULL, "cannot write the ffmpeg command");
      status = 2;
      ok = 0;
    }
  }
  source_close(&src);
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
   (fftools enc_open): the frame properties, the time base 1/fr (video) or
   1/sample_rate (audio), the global header for mp4, threads=auto. */
static AVCodecContext *open_encoder(const AVFormatContext *oc, enum AVMediaType type, const char *path, const AVCodecContext *dec, AVRational fr) {
  const AVCodec *codec = avcodec_find_encoder(av_guess_codec(oc->oformat, NULL, path, NULL, type));
  AVCodecContext *enc = codec == NULL ? NULL : avcodec_alloc_context3(codec);
  AVDictionary *opts = NULL;
  if (enc == NULL)
    return NULL;
  int ok = 1;
  if (type == AVMEDIA_TYPE_VIDEO) {
    enc->width = dec->width;
    enc->height = dec->height;
    enc->pix_fmt = dec->pix_fmt;
    enc->sample_aspect_ratio = dec->sample_aspect_ratio;
    enc->color_range = dec->color_range;
    enc->color_primaries = dec->color_primaries;
    enc->color_trc = dec->color_trc;
    enc->colorspace = dec->colorspace;
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

/* Opens the decoders, the encoders, the muxer and the output file. Returns
   NULL, or the reason of the failure. */
static const char *enc_open(Enc *e, const Source *s, const char *path, const char *io_path) {
  if (avformat_alloc_output_context2(&e->oc, NULL, NULL, path) < 0 || e->oc == NULL)
    return "no muxer for the output name";
  e->vdec = open_decoder(s->fmt->streams[s->vs]);
  e->adec = s->as < 0 ? NULL : open_decoder(s->fmt->streams[s->as]);
  if (e->vdec == NULL || (s->as >= 0 && e->adec == NULL))
    return "cannot open a decoder";
  e->venc = open_encoder(e->oc, AVMEDIA_TYPE_VIDEO, path, e->vdec, s->fr);
  e->aenc = e->adec == NULL ? NULL : open_encoder(e->oc, AVMEDIA_TYPE_AUDIO, path, e->adec, s->fr);
  if (e->venc == NULL || (e->adec != NULL && e->aenc == NULL))
    return "cannot open an encoder";
  e->vst = add_stream(e->oc, e->venc);
  e->ast = e->aenc == NULL ? NULL : add_stream(e->oc, e->aenc);
  if (e->vst == NULL || (e->aenc != NULL && e->ast == NULL))
    return "cannot add an output stream";
  e->vst->avg_frame_rate = s->fr;
  if (e->aenc != NULL) {
    int size = e->aenc->frame_size > 0 ? e->aenc->frame_size : 1024;
    e->fifo = av_audio_fifo_alloc(e->aenc->sample_fmt, e->aenc->ch_layout.nb_channels, size);
  }
  e->pkt = av_packet_alloc();
  e->frame = av_frame_alloc();
  e->chunk = av_frame_alloc();
  if (e->pkt == NULL || e->frame == NULL || e->chunk == NULL || (e->aenc != NULL && e->fifo == NULL))
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
  av_packet_free(&e->pkt);
  av_frame_free(&e->frame);
  av_frame_free(&e->chunk);
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

/* Each kept frame gets pts = its rank in the cut (O-2, setpts=PTS-STARTPTS)
   and loses the decoder picture type, which libx264 would force. */
static int video_frames(Enc *e) {
  int r = 0;
  while (r >= 0 && (r = avcodec_receive_frame(e->vdec, e->frame)) >= 0) {
    uint64_t p = e->position++;
    if (p >= e->cut.keep.lo && p <= e->cut.keep.hi) {
      e->frame->pts = (int64_t)(p - e->cut.keep.lo);
      e->frame->duration = 1;
      e->frame->time_base = e->venc->time_base;
      e->frame->pict_type = AV_PICTURE_TYPE_NONE;
      e->frame->flags &= ~AV_FRAME_FLAG_KEY;
      r = send(e, e->venc, e->vst, e->frame);
    }
    av_frame_unref(e->frame);
  }
  return drained(r);
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
  f->pts = e->apts;
  f->duration = n;
  f->time_base = e->aenc->time_base;
  e->apts += n;
  return send(e, e->aenc, e->ast, f);
}

/* Keeps the samples of F inside [astart, aend) (counted from the first
   decoded sample, P7) and encodes every full frame. */
static int audio_keep(Enc *e, const AVFrame *f) {
  int64_t lo = e->seen;
  int64_t hi = e->seen + f->nb_samples;
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
  int size = e->aenc->frame_size > 0 ? e->aenc->frame_size : 1024;
  int r = 0;
  while (r >= 0 && av_audio_fifo_size(e->fifo) >= size)
    r = audio_chunk(e, size);
  return r;
}

static int audio_frames(Enc *e) {
  int r = 0;
  while (r >= 0 && (r = avcodec_receive_frame(e->adec, e->frame)) >= 0) {
    r = audio_keep(e, e->frame);
    av_frame_unref(e->frame);
  }
  return drained(r);
}

/* Decodes every packet in file order, then flushes the decoders, the last
   short audio frame and the encoders. */
static int enc_run(Enc *e, const Source *s) {
  int r = 0;
  for (size_t i = 0; r >= 0 && i < s->pkt_count; i++) {
    int video = s->pkts[i]->stream_index == s->vs;
    r = avcodec_send_packet(video ? e->vdec : e->adec, s->pkts[i]);
    if (r >= 0)
      r = video ? video_frames(e) : audio_frames(e);
  }
  if (r >= 0 && (r = avcodec_send_packet(e->vdec, NULL)) >= 0)
    r = video_frames(e);
  if (r >= 0 && e->adec != NULL && (r = avcodec_send_packet(e->adec, NULL)) >= 0)
    r = audio_frames(e);
  if (r >= 0 && e->aenc != NULL && av_audio_fifo_size(e->fifo) > 0)
    r = audio_chunk(e, av_audio_fifo_size(e->fifo));
  if (r >= 0)
    r = send(e, e->venc, e->vst, NULL);
  if (r >= 0 && e->aenc != NULL)
    r = send(e, e->aenc, e->ast, NULL);
  return r;
}

static int encode(Machine *m, const Source *s, const Cut *cut, const char *path) {
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
  e.cut = *cut;
  const char *why = enc_open(&e, s, path, temporary);
  if (why == NULL && enc_run(&e, s) < 0)
    why = "an encode step failed";
  if (why == NULL && av_write_trailer(e.oc) < 0)
    why = "cannot write the container trailer";
  int count_ok = why != NULL || e.position == s->frames;
  uint64_t decoded = e.position;
  if (enc_free(&e) < 0 && why == NULL)
    why = "cannot close the output file";
  if (why == NULL && count_ok && rename(temporary, path) != 0)
    why = "cannot replace the output file";
  remove(temporary);
  free(temporary);
  if (why != NULL)
    return diag_fail(m->diag, "MEDIA_ENCODE", NULL, "%s: %s", path, why) + 2;
  if (!count_ok)
    return diag_fail(m->diag, "MEDIA_DECODE", NULL, "decoded %llu frames, the index has %llu",
                     (unsigned long long)decoded, (unsigned long long)s->frames) + 1;
  return 0;
}

int media_build(Machine *m, const char *entry, char *const *inputs, int input_count, const char *out_path) {
  Source src;
  Cut cut;
  int status = 1;
  char vf[FILTER_MAX];
  char af[FILTER_MAX];
  int ok = prepare(m, entry, inputs, input_count, &src, vf, af, &cut, &status);
  int result = ok ? encode(m, &src, &cut, out_path) : status;
  source_close(&src);
  return result;
}
