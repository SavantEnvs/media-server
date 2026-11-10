// Fuzzes the FLV tag demuxer (libflv flv_demuxer_input): a sequence of [type][len:2][tag body] records,
// type = 8 audio / 9 video / 18 script, so sequence headers (AVCDecoderConfigurationRecord, AudioSpecificConfig,
// HEVC/VVC/AV1 configs) can precede the frames that depend on them.
#include "flv-demuxer.h"
#include "records.h"

static int on_packet(void* param, int codec, const void* data, size_t bytes, uint32_t pts, uint32_t dts, int flags)
{
	(void)param; (void)codec; (void)pts; (void)dts; (void)flags;
	touch(data, bytes);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	static const int types[3] = { 8, 9, 18 };
	const uint8_t* rec;
	size_t len;
	uint32_t ts = 0;
	struct flv_demuxer_t* demux;

	if (size < 3)
		return -1;
	demux = flv_demuxer_create(on_packet, NULL);
	if (!demux)
		return 0;
	while (size > 0)
	{
		int type = types[data[0] % 3];
		data++;
		size--;
		if (!next_record(&data, &size, &rec, &len))
			break;
		if (flv_demuxer_input(demux, type, rec, len, ts) < 0)
			break;
		ts += 40;
	}
	flv_demuxer_destroy(demux);
	return 0;
}
