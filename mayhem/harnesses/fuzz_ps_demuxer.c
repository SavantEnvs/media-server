// Fuzzes the MPEG-2 program stream demuxer (libmpeg ps_demuxer_input): pack/system headers, PSM, PES and the
// per-codec frame splitters, driven the way the API documents it (consume, keep the remainder).
#include "mpeg-ps.h"
#include "records.h"

static int on_packet(void* param, int stream, int codecid, int flags, int64_t pts, int64_t dts, const void* data, size_t bytes)
{
	(void)param; (void)stream; (void)codecid; (void)flags; (void)pts; (void)dts;
	touch(data, bytes);
	return 0;
}

static void on_stream(void* param, int stream, int codecid, const void* extra, int bytes, int finish)
{
	(void)param; (void)stream; (void)codecid; (void)finish;
	if (bytes > 0)
		touch(extra, (size_t)bytes);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	struct ps_demuxer_notify_t notify = { on_stream };
	struct ps_demuxer_t* demux;
	size_t off = 0;

	if (size < 4)
		return -1;
	demux = ps_demuxer_create(on_packet, NULL);
	if (!demux)
		return 0;
	ps_demuxer_set_notify(demux, &notify, NULL);
	while (off < size)
	{
		int n = ps_demuxer_input(demux, data + off, size - off);
		if (n <= 0)
			break;
		off += (size_t)n;
	}
	ps_demuxer_destroy(demux);
	return 0;
}
