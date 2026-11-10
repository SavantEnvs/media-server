// Fuzzes the RTP payload depacketizers (librtp rtp_payload_decode_input): the first input byte picks the codec
// (H.264/H.265/H.266/AV1/VP8/VP9/MPEG-4 ES/LATM/generic/MPEG-1/2 ES/TS/PS/PCM/opus ...), the rest is a sequence of
// [len:2][RTP packet] records fed to one decoder, so fragmentation (FU-A, aggregation) state carries across packets.
#include "rtp-payload.h"
#include "records.h"
#include <stdlib.h>

struct choice
{
	int payload;
	const char* name;
};

static const struct choice kChoices[] = {
	{ 96, "H264" }, { 97, "H265" }, { 98, "H266" }, { 99, "MP4V-ES" }, { 100, "MP4A-LATM" },
	{ 101, "mpeg4-generic" }, { 102, "VP8" }, { 103, "VP9" }, { 104, "AV1" }, { 105, "PS" },
	{ 106, "MP1S" }, { 107, "opus" }, { 108, "G726-32" }, { 0, "PCMU" }, { 8, "PCMA" },
	{ 14, "MPA" }, { 32, "MPV" }, { 33, "MP2T" }, { 9, "G722" }, { 18, "G729" },
};

static void* on_alloc(void* param, int bytes)
{
	(void)param;
	return malloc(bytes > 0 ? (size_t)bytes : 1);
}

static void on_free(void* param, void* packet)
{
	(void)param;
	free(packet);
}

static int on_packet(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
	(void)param; (void)timestamp; (void)flags;
	if (bytes > 0)
		touch(packet, (size_t)bytes);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	struct rtp_payload_t handler = { on_alloc, on_free, on_packet };
	const struct choice* ch;
	const uint8_t* rec;
	size_t len;
	void* decoder;

	if (size < 13) // selector + a 12-byte RTP header
		return -1;
	ch = &kChoices[data[0] % (sizeof(kChoices) / sizeof(kChoices[0]))];
	data++;
	size--;
	decoder = rtp_payload_decode_create(ch->payload, ch->name, &handler, NULL);
	if (!decoder)
		return 0;
	while (next_record(&data, &size, &rec, &len))
		if (rtp_payload_decode_input(decoder, rec, (int)len) < 0)
			break;
	rtp_payload_decode_destroy(decoder);
	return 0;
}
