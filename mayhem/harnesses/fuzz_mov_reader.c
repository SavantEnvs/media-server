// Fuzzes the MP4/MOV/fMP4 demuxer (libmov mov_reader_*): box tree, sample tables, track info and frame reads.
#include "mov-reader.h"
#include "mov-format.h"
#include "membuf.h"
#include "records.h"
#include <stdlib.h>

static const struct mov_buffer_t s_io = { membuf_read, membuf_write, membuf_seek, membuf_tell };
static const size_t kMaxSample = 16u << 20;

struct alloc_ctx
{
	void* ptr;
};

static void on_video(void* param, uint32_t track, uint8_t object, int width, int height, const void* extra, size_t bytes)
{
	(void)param; (void)track; (void)object; (void)width; (void)height;
	touch(extra, bytes);
}

static void on_audio(void* param, uint32_t track, uint8_t object, int channels, int bits, int rate, const void* extra, size_t bytes)
{
	(void)param; (void)track; (void)object; (void)channels; (void)bits; (void)rate;
	touch(extra, bytes);
}

static void on_subtitle(void* param, uint32_t track, uint8_t object, const void* extra, size_t bytes)
{
	(void)param; (void)track; (void)object;
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
	struct mov_reader_trackinfo_t info = { on_video, on_audio, on_subtitle };
	struct alloc_ctx c = { NULL };
	mov_reader_t* mov;
	int64_t ts;

	if (size < 8) // smallest box: 4-byte size + 4-byte type
		return -1;
	mov = mov_reader_create(&s_io, &m);
	if (!mov)
		return 0;
	if (0 == mov_reader_getinfo(mov, &info, NULL))
	{
		(void)mov_reader_getduration(mov);
		while (mov_reader_read2(mov, on_alloc, &c) > 0)
			touch(c.ptr, 1);
		ts = (int64_t)(size & 0xFFFF) * 10;
		if (0 == mov_reader_seek(mov, &ts))
			(void)mov_reader_read2(mov, on_alloc, &c);
	}
	free(c.ptr);
	mov_reader_destroy(mov);
	return 0;
}
