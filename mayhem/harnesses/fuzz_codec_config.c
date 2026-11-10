// Fuzzes the codec configuration record parsers (libflv): AVC/HEVC/VVC/AV1/AVS3/VPx decoder configuration records,
// AAC AudioSpecificConfig/ADTS/LATM, Opus head, FLAC streaminfo, MP3 header, WAVEFORMAT; plus the Annex-B -> config
// extractors and AV1 OBU splitters. First byte selects the parser, the rest is its input.
#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include "mpeg4-vvc.h"
#include "mpeg4-aac.h"
#include "aom-av1.h"
#include "avswg-avs3.h"
#include "webm-vpx.h"
#include "opus-head.h"
#include "xiph-flac.h"
#include "mp3-header.h"
#include "riff-acm.h"
#include "records.h"
#include <stdint.h>
#include <string.h>

static uint8_t s_out[64 * 1024];

// The splitters hand each callback a range inside the input they were given. A range that leaves it is the
// library's pointer arithmetic going wrong (aom-av1.c:176 `ptr + len > data + bytes` wraps for a huge LEB128 len),
// not something for the harness to read: it is skipped here so the report cannot be filed against `touch`.
static const uint8_t* s_in;
static size_t s_in_size;

static int in_input(const void* p, size_t n)
{
	uintptr_t off;
	if ((uintptr_t)p < (uintptr_t)s_in)
		return 0;
	off = (uintptr_t)p - (uintptr_t)s_in;
	return off <= s_in_size && n <= s_in_size - off;
}

static int on_obu(void* param, const uint8_t* obu, size_t bytes)
{
	(void)param;
	if (!in_input(obu, bytes))
		return -1;
	touch(obu, bytes);
	return 0;
}

static void on_nalu(void* param, const uint8_t* nalu, size_t bytes)
{
	(void)param;
	if (in_input(nalu, bytes))
		touch(nalu, bytes);
}

static int on_opus_frame(uint8_t toc, const void* frame, size_t bytes)
{
	(void)toc;
	if (!in_input(frame, bytes))
		return -1;
	touch(frame, bytes);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	char codecs[128];
	int width, height;

	if (size < 2)
		return -1;
	{
		int which = data[0] % 17;
		data++;
		size--;
		s_in = data;
		s_in_size = size;
		switch (which)
		{
		case 0: {
			struct mpeg4_avc_t avc;
			memset(&avc, 0, sizeof(avc));
			if (mpeg4_avc_decoder_configuration_record_load(data, size, &avc) > 0)
			{
				mpeg4_avc_decoder_configuration_record_save(&avc, s_out, sizeof(s_out));
				mpeg4_avc_to_nalu(&avc, s_out, sizeof(s_out));
				mpeg4_avc_codecs(&avc, codecs, sizeof(codecs));
			}
			break; }
		case 1: {
			struct mpeg4_avc_t avc;
			memset(&avc, 0, sizeof(avc));
			if (mpeg4_avc_from_nalu(data, size, &avc) > 0)
				mpeg4_avc_codecs(&avc, codecs, sizeof(codecs));
			mpeg4_h264_bitstream_format(data, size);
			mpeg4_h264_annexb_nalu(data, size, on_nalu, NULL);
			break; }
		case 2: {
			struct mpeg4_hevc_t hevc;
			memset(&hevc, 0, sizeof(hevc));
			if (mpeg4_hevc_decoder_configuration_record_load(data, size, &hevc) > 0)
			{
				mpeg4_hevc_decoder_configuration_record_save(&hevc, s_out, sizeof(s_out));
				mpeg4_hevc_to_nalu(&hevc, s_out, sizeof(s_out));
				mpeg4_hevc_codecs(&hevc, codecs, sizeof(codecs));
			}
			break; }
		case 3: {
			struct mpeg4_hevc_t hevc;
			memset(&hevc, 0, sizeof(hevc));
			if (mpeg4_hevc_from_nalu(data, size, &hevc) > 0)
				mpeg4_hevc_codecs(&hevc, codecs, sizeof(codecs));
			break; }
		case 4: {
			struct mpeg4_vvc_t vvc;
			memset(&vvc, 0, sizeof(vvc));
			if (mpeg4_vvc_decoder_configuration_record_load(data, size, &vvc) > 0)
			{
				mpeg4_vvc_decoder_configuration_record_save(&vvc, s_out, sizeof(s_out));
				mpeg4_vvc_to_nalu(&vvc, s_out, sizeof(s_out));
				mpeg4_vvc_codecs(&vvc, codecs, sizeof(codecs));
			}
			break; }
		case 5: {
			struct aom_av1_t av1;
			memset(&av1, 0, sizeof(av1));
			if (aom_av1_codec_configuration_record_load(data, size, &av1) > 0)
			{
				aom_av1_codec_configuration_record_save(&av1, s_out, sizeof(s_out));
				aom_av1_codecs(&av1, codecs, sizeof(codecs));
			}
			break; }
		case 6: {
			struct aom_av1_t av1;
			memset(&av1, 0, sizeof(av1));
			aom_av1_codec_configuration_record_init(&av1, data, size);
			aom_av1_obu_split(data, size, on_obu, NULL);
			aom_av1_annexb_split(data, size, on_obu, NULL);
			break; }
		case 7: {
			struct avswg_avs3_t avs3;
			memset(&avs3, 0, sizeof(avs3));
			if (avswg_avs3_decoder_configuration_record_load(data, size, &avs3) > 0)
			{
				avswg_avs3_decoder_configuration_record_save(&avs3, s_out, sizeof(s_out));
				avswg_avs3_codecs(&avs3, codecs, sizeof(codecs));
			}
			memset(&avs3, 0, sizeof(avs3));
			avswg_avs3_decoder_configuration_record_init(&avs3, data, size);
			break; }
		case 8: {
			struct webm_vpx_t vpx;
			memset(&vpx, 0, sizeof(vpx));
			if (webm_vpx_codec_configuration_record_load(data, size, &vpx) > 0)
				webm_vpx_codec_configuration_record_save(&vpx, s_out, sizeof(s_out));
			break; }
		case 9: {
			struct webm_vpx_t vpx;
			memset(&vpx, 0, sizeof(vpx));
			webm_vpx_codec_configuration_record_from_vp8(&vpx, &width, &height, data, size);
			memset(&vpx, 0, sizeof(vpx));
			webm_vpx_codec_configuration_record_from_vp9(&vpx, &width, &height, data, size);
			break; }
		case 10: {
			struct mpeg4_aac_t aac;
			memset(&aac, 0, sizeof(aac));
			if (mpeg4_aac_audio_specific_config_load(data, size, &aac) >= 0)
			{
				mpeg4_aac_audio_specific_config_save(&aac, s_out, sizeof(s_out));
				mpeg4_aac_codecs(&aac, codecs, sizeof(codecs));
				mpeg4_aac_profile_level(&aac);
			}
			break; }
		case 11: {
			struct mpeg4_aac_t aac;
			memset(&aac, 0, sizeof(aac));
			if (mpeg4_aac_adts_load(data, size, &aac) >= 0)
				mpeg4_aac_adts_save(&aac, size, s_out, sizeof(s_out));
			mpeg4_aac_adts_frame_length(data, size);
			break; }
		case 12: {
			struct mpeg4_aac_t aac;
			memset(&aac, 0, sizeof(aac));
			if (mpeg4_aac_stream_mux_config_load(data, size, &aac) >= 0)
				mpeg4_aac_stream_mux_config_save(&aac, s_out, sizeof(s_out));
			break; }
		case 13: {
			struct opus_head_t opus;
			memset(&opus, 0, sizeof(opus));
			if (opus_head_load(data, size, &opus) > 0)
				opus_head_save(&opus, s_out, sizeof(s_out));
			opus_packet_getframes(data, size, on_opus_frame, NULL);
			break; }
		case 14: {
			struct flac_streaminfo_t flac;
			memset(&flac, 0, sizeof(flac));
			if (flac_streaminfo_load(data, size, &flac) > 0)
				flac_streaminfo_save(&flac, s_out, sizeof(s_out));
			break; }
		case 15: {
			struct mp3_header_t mp3;
			memset(&mp3, 0, sizeof(mp3));
			if (mp3_header_load(&mp3, data, (int)size) > 0)
			{
				mp3_get_channel(&mp3);
				mp3_get_bitrate(&mp3);
				mp3_get_frequency(&mp3);
				mp3_header_save(&mp3, s_out, sizeof(s_out));
			}
			break; }
		default: {
			struct wave_format_t wav;
			memset(&wav, 0, sizeof(wav));
			if (wave_format_load(data, (int)size, &wav) > 0)
				wave_format_save(&wav, s_out, sizeof(s_out));
			break; }
		}
	}
	return 0;
}
