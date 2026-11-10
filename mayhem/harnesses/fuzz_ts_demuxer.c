// Fuzzes the MPEG-2 transport stream demuxer (libmpeg ts_demuxer_input): PAT/PMT/SDT, PES reassembly, adaptation
// fields and the per-codec elementary stream frame splitters.
#include "mpeg-ts.h"
#include "records.h"

#define TS_PACKET 188

static int on_packet(void* param, int program, int stream, int codecid, int flags, int64_t pts, int64_t dts, const void* data, size_t bytes)
{
	(void)param; (void)program; (void)stream; (void)codecid; (void)flags; (void)pts; (void)dts;
	touch(data, bytes);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	struct ts_demuxer_t* demux;
	size_t off;

	// the demuxer's input contract is whole 188-byte packets (assert(188 == bytes)); the sync byte is not
	// pre-filtered, because the build is -DNDEBUG and the demuxer's assert(0x47 == data[0]) is compiled out
	if (size < TS_PACKET)
		return -1;

	demux = ts_demuxer_create(on_packet, NULL);
	if (!demux)
		return 0;
	for (off = 0; off + TS_PACKET <= size; off += TS_PACKET)
		if (ts_demuxer_input(demux, data + off, TS_PACKET) < 0)
			break;
	ts_demuxer_flush(demux);
	ts_demuxer_destroy(demux);
	return 0;
}
