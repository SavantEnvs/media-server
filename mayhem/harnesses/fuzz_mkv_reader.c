// Fuzzes the Matroska/WebM demuxer (libmkv mkv_reader_*): EBML header, segment/cluster/cue parsing, track info
// and frame reads.
#include "mkv-reader.h"
#include "membuf.h"
#include "records.h"
#include <stdlib.h>

static const struct mkv_buffer_t s_io = { membuf_read, membuf_write, membuf_seek, membuf_tell };
static const size_t kMaxSample = 16u << 20;

struct alloc_ctx
{
	void* ptr;
};

static void on_video(void* param, uint32_t track, enum mkv_codec_t codec, int width, int height, const void* extra, size_t bytes)
{
	(void)param; (void)track; (void)codec; (void)width; (void)height;
	touch(extra, bytes);
}

static void on_audio(void* param, uint32_t track, enum mkv_codec_t codec, int channels, int bits, int rate, const void* extra, size_t bytes)
{
	(void)param; (void)track; (void)codec; (void)channels; (void)bits; (void)rate;
	touch(extra, bytes);
}

static void on_subtitle(void* param, uint32_t track, enum mkv_codec_t codec, const void* extra, size_t bytes)
{
	(void)param; (void)track; (void)codec;
	touch(extra, bytes);
}

static void* on_alloc(void* param, uint32_t track, size_t bytes, int64_t pts, int64_t dts, int flags)
{
	struct alloc_ctx* c = (struct alloc_ctx*)param;
	(void)track; (void)pts; (void)dts; (void)flags;
	if (bytes > kMaxSample)
		return NULL;
	free(c->ptr);
	c->ptr = malloc(bytes ? bytes : 1);
	return c->ptr;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	struct membuf m = { data, size, 0 };
	struct mkv_reader_trackinfo_t info = { on_video, on_audio, on_subtitle };
	struct alloc_ctx c = { NULL };
	mkv_reader_t* mkv;
	int64_t ts;

	if (size < 4) // EBML magic is 4 bytes
		return -1;
	mkv = mkv_reader_create(&s_io, &m);
	if (!mkv)
		return 0;
	if (0 == mkv_reader_getinfo(mkv, &info, NULL))
	{
		(void)mkv_reader_getduration(mkv);
		while (mkv_reader_read2(mkv, on_alloc, &c) > 0)
			touch(c.ptr, 1);
		ts = (int64_t)(size & 0xFFFF) * 10;
		if (0 == mkv_reader_seek(mkv, &ts))
			(void)mkv_reader_read2(mkv, on_alloc, &c);
	}
	free(c.ptr);
	mkv_reader_destroy(mkv);
	return 0;
}
