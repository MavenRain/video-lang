/* video-lang K0: the media back end of langc (demux, trim, encode). */
#ifndef LANG_MEDIA_H
#define LANG_MEDIA_H

#include <stdio.h>

#include "front/core.h"

/* `langc build PROG MAIN IN... -o OUT` (O-1): encodes MAIN applied to the
   inputs. Returns the exit status: 0 ok, 1 refused, 2 usage or IO. A refused
   or failed build preserves an existing OUT and creates no new output. */
int media_build(Machine *m, const char *entry, char *const *inputs, int input_count, const char *out_path);

/* `langc ffmpeg PROG MAIN IN... -o OUT`: prints the reference ffmpeg argv of
   the same build on OUT. Same exit statuses. */
int media_ffmpeg(Machine *m, const char *entry, char *const *inputs, int input_count, const char *out_path, FILE *out);

#endif
