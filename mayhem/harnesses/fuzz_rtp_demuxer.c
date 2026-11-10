// Fuzzes the RTP receive path (librtp rtp_demuxer_input): RTP header/CSRC/extension parsing, reorder jitter buffer,
// source tracking, and RTCP compound packets (SR/RR/SDES/BYE/APP/XR/RTPFB/PSFB), then the RTCP report generator.
#include "rtp-demuxer.h"
#include "records.h"

static int on_packet(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
	(void)param; (void)timestamp; (void)flags;
	if (bytes > 0)
		touch(packet, (size_t)bytes);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	struct rtp_demuxer_t* demux;
	const uint8_t* rec;
	size_t len;
	uint8_t report[1500];
	int lost, late, misorder, duplicate;

	if (size < 5)
		return -1;
	// PCMU keeps the depacketizer trivial so the fuzzing stays on the receive path; byte 0 is skipped
	demux = rtp_demuxer_create(200, 8000, 0, "PCMU", on_packet, NULL);
	if (!demux)
		return 0;
	data++;
	size--;
	while (next_record(&data, &size, &rec, &len))
	{
		if (rtp_demuxer_input(demux, rec, (int)len) < 0)
			continue;
	}
	(void)rtp_demuxer_rtcp(demux, report, sizeof(report));
	rtp_demuxer_stats(demux, &lost, &late, &misorder, &duplicate);
	rtp_demuxer_destroy(&demux);
	return 0;
}
