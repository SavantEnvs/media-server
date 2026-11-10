// Known-answer probe for mayhem/test.sh. Builds a deterministic synthetic H.264 + AAC stream, pushes it through the
// real muxers and demuxers of libmov, libmkv, libflv, libmpeg and librtp, and prints what comes out (counts,
// timestamps, flags, CRC-32 of every payload). test.sh compares the output line by line with mayhem/kat/expected.txt.
//
// `media_server_kat <dir>` additionally writes the muxed streams into <dir>; that is how the starter seeds under
// mayhem/<target>/testsuite/ were generated.
#include "mov-reader.h"
#include "mov-writer.h"
#include "fmp4-writer.h"
#include "mov-format.h"
#include "mkv-reader.h"
#include "mkv-writer.h"
#include "mkv-format.h"
#include "flv-muxer.h"
#include "flv-demuxer.h"
#include "flv-parser.h"
#include "flv-proto.h"
#include "flv-header.h"
#include "mpeg4-avc.h"
#include "mpeg4-aac.h"
#include "mpeg-ts.h"
#include "mpeg-ps.h"
#include "rtp-payload.h"
#include "rtp-demuxer.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* g_dir;

static uint32_t crc32(const void* data, size_t n)
{
	const uint8_t* p = (const uint8_t*)data;
	uint32_t c = 0xFFFFFFFFu;
	size_t i;
	int k;
	for (i = 0; i < n; i++)
	{
		c ^= p[i];
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

struct gbuf
{
	uint8_t* p;
	uint64_t n, cap, off;
};

static int gb_read(void* param, void* data, uint64_t bytes)
{
	struct gbuf* g = (struct gbuf*)param;
	if (bytes > g->n - g->off)
		return -E2BIG;
	memcpy(data, g->p + g->off, (size_t)bytes);
	g->off += bytes;
	return 0;
}

static int gb_write(void* param, const void* data, uint64_t bytes)
{
	struct gbuf* g = (struct gbuf*)param;
	if (g->off + bytes > g->cap)
	{
		uint64_t cap = (g->off + bytes) * 2 + 4096;
		uint8_t* p = (uint8_t*)realloc(g->p, (size_t)cap);
		if (!p)
			return -ENOMEM;
		g->p = p;
		g->cap = cap;
	}
	if (g->off > g->n)
		memset(g->p + g->n, 0, (size_t)(g->off - g->n));
	memcpy(g->p + g->off, data, (size_t)bytes);
	g->off += bytes;
	if (g->off > g->n)
		g->n = g->off;
	return 0;
}

static int gb_seek(void* param, int64_t offset)
{
	struct gbuf* g = (struct gbuf*)param;
	if (offset >= 0)
	{
		g->off = (uint64_t)offset; // like fseek: past the end is allowed, the gap reads back as zeros once written
	}
	else
	{
		if ((uint64_t)(-offset) > g->n)
			return -E2BIG;
		g->off = g->n - (uint64_t)(-offset);
	}
	return 0;
}

static int64_t gb_tell(void* param)
{
	return (int64_t)((struct gbuf*)param)->off;
}

static const struct mov_buffer_t kMovIo = { gb_read, gb_write, gb_seek, gb_tell };
static const struct mkv_buffer_t kMkvIo = { gb_read, gb_write, gb_seek, gb_tell };

static void emit(const char* name, const void* data, size_t n)
{
	char path[512];
	FILE* fp;
	if (!g_dir)
		return;
	snprintf(path, sizeof(path), "%s/%s", g_dir, name);
	fp = fopen(path, "wb");
	if (!fp || fwrite(data, 1, n, fp) != n)
	{
		fprintf(stderr, "cannot write %s\n", path);
		exit(2);
	}
	fclose(fp);
}

// ---- synthetic elementary streams -------------------------------------------------------------------------------

static const uint8_t kSps[] = { 0x67, 0x64, 0x00, 0x0a, 0xac, 0xd9, 0x44, 0x26, 0x84, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xca, 0x3c, 0x48, 0x96, 0x11, 0x80 };
static const uint8_t kPps[] = { 0x68, 0xe8, 0x43, 0x8f, 0x13, 0x21, 0x30 };
static const uint8_t kAsc[] = { 0x12, 0x10 };
static const uint8_t kStart[] = { 0, 0, 0, 1 };

#define NFRAMES 6
#define NAUDIO 8

static size_t pat(uint8_t* out, int seed, size_t n)
{
	size_t j;
	for (j = 0; j < n; j++)
		out[j] = (uint8_t)((j * 7 + (size_t)seed * 13) % 251 + 1); // never 0: no stray start codes
	return n;
}

// Annex-B access unit i: key frame (i == 0) carries SPS+PPS+IDR, the others one P slice.
static size_t make_au(uint8_t* out, int i)
{
	size_t n = 0;
	if (0 == i)
	{
		memcpy(out + n, kStart, 4); n += 4;
		memcpy(out + n, kSps, sizeof(kSps)); n += sizeof(kSps);
		memcpy(out + n, kStart, 4); n += 4;
		memcpy(out + n, kPps, sizeof(kPps)); n += sizeof(kPps);
		memcpy(out + n, kStart, 4); n += 4;
		out[n++] = 0x65;
		n += pat(out + n, i, 300);
	}
	else
	{
		memcpy(out + n, kStart, 4); n += 4;
		out[n++] = 0x41;
		n += pat(out + n, i, 120 + (size_t)i * 17);
	}
	return n;
}

static size_t make_aac(uint8_t* out, int i) // raw AAC frame
{
	return pat(out, 100 + i, 24 + (size_t)i * 3);
}

static size_t make_adts(uint8_t* out, int i)
{
	struct mpeg4_aac_t aac;
	size_t n = make_aac(out + 7, i);
	memset(&aac, 0, sizeof(aac));
	aac.profile = MPEG4_AAC_LC;
	aac.sampling_frequency_index = 4;
	aac.channel_configuration = 2;
	aac.sampling_frequency = 44100;
	aac.channels = 2;
	if (mpeg4_aac_adts_save(&aac, n, out, 7) != 7)
		abort();
	return n + 7;
}

static int vts(int i) { return i * 40; }
static int ats(int i) { return i * 23; }

// ---- 1. codec config records ------------------------------------------------------------------------------------

static void kat_config(void)
{
	struct mpeg4_aac_t aac;
	struct mpeg4_avc_t avc;
	uint8_t annexb[128], rec[256];
	size_t n = 0;
	int r;

	memset(&aac, 0, sizeof(aac));
	r = mpeg4_aac_audio_specific_config_load(kAsc, sizeof(kAsc), &aac);
	printf("aac.asc: r=%d profile=%u freq=%u ch=%u\n", r, aac.profile, aac.sampling_frequency, aac.channels);

	memcpy(annexb + n, kStart, 4); n += 4;
	memcpy(annexb + n, kSps, sizeof(kSps)); n += sizeof(kSps);
	memcpy(annexb + n, kStart, 4); n += 4;
	memcpy(annexb + n, kPps, sizeof(kPps)); n += sizeof(kPps);
	memset(&avc, 0, sizeof(avc));
	r = mpeg4_avc_from_nalu(annexb, n, &avc);
	printf("avc.from_nalu: r=%d profile=%u level=%u sps=%u pps=%u\n", r, avc.profile, avc.level, avc.nb_sps, avc.nb_pps);
	r = mpeg4_avc_decoder_configuration_record_save(&avc, rec, sizeof(rec));
	printf("avc.record: bytes=%d crc=%08x\n", r, r > 0 ? crc32(rec, (size_t)r) : 0);
	emit("avc-record.bin", rec, r > 0 ? (size_t)r : 0);
	memset(&avc, 0, sizeof(avc));
	r = r > 0 ? mpeg4_avc_decoder_configuration_record_load(rec, (size_t)r, &avc) : -1;
	printf("avc.record_load: r=%d profile=%u level=%u sps=%u pps=%u\n", r, avc.profile, avc.level, avc.nb_sps, avc.nb_pps);
}

// ---- 2. MP4 / fMP4 / Matroska: mux in memory, demux, compare -----------------------------------------------------

struct rd
{
	uint8_t* buf;
	size_t cap;
	int track;
	int64_t pts, dts;
	int flags;
	size_t bytes;
	int nvideo, naudio, nsub;
	int width, height, channels, rate;
};

static void* rd_alloc(void* param, uint32_t track, size_t bytes, int64_t pts, int64_t dts, int flags)
{
	struct rd* r = (struct rd*)param;
	if (bytes > r->cap)
		return NULL;
	r->track = (int)track;
	r->bytes = bytes;
	r->pts = pts;
	r->dts = dts;
	r->flags = flags;
	return r->buf;
}

static void mov_video(void* p, uint32_t t, uint8_t o, int w, int h, const void* e, size_t n) { struct rd* r = (struct rd*)p; (void)t; (void)o; (void)e; (void)n; r->nvideo++; r->width = w; r->height = h; }
static void mov_audio(void* p, uint32_t t, uint8_t o, int c, int b, int s, const void* e, size_t n) { struct rd* r = (struct rd*)p; (void)t; (void)o; (void)b; (void)e; (void)n; r->naudio++; r->channels = c; r->rate = s; }
static void mov_sub(void* p, uint32_t t, uint8_t o, const void* e, size_t n) { struct rd* r = (struct rd*)p; (void)t; (void)o; (void)e; (void)n; r->nsub++; }
static void mkv_video(void* p, uint32_t t, enum mkv_codec_t o, int w, int h, const void* e, size_t n) { struct rd* r = (struct rd*)p; (void)t; (void)o; (void)e; (void)n; r->nvideo++; r->width = w; r->height = h; }
static void mkv_audio(void* p, uint32_t t, enum mkv_codec_t o, int c, int b, int s, const void* e, size_t n) { struct rd* r = (struct rd*)p; (void)t; (void)o; (void)b; (void)e; (void)n; r->naudio++; r->channels = c; r->rate = s; }
static void mkv_sub(void* p, uint32_t t, enum mkv_codec_t o, const void* e, size_t n) { struct rd* r = (struct rd*)p; (void)t; (void)o; (void)e; (void)n; r->nsub++; }

// writer frames must be length-prefixed (MP4 sample) form
static size_t to_avcc(struct mpeg4_avc_t* avc, uint8_t* out, size_t cap, const uint8_t* annexb, size_t n, int* vcl)
{
	int update = 0;
	int r = h264_annexbtomp4(avc, annexb, n, out, cap, vcl, &update);
	return r > 0 ? (size_t)r : 0;
}

static void print_info(const char* name, const struct rd* r)
{
	printf("%s.info: video=%d %dx%d audio=%d ch=%d rate=%d sub=%d\n", name, r->nvideo, r->width, r->height, r->naudio, r->channels, r->rate, r->nsub);
}

static void print_sample(const char* name, int i, int rc, const struct rd* r)
{
	printf("%s.sample.%d: rc=%d track=%d bytes=%zu pts=%lld dts=%lld flags=%d crc=%08x\n", name, i, rc, r->track, r->bytes,
		(long long)r->pts, (long long)r->dts, r->flags, rc > 0 ? crc32(r->buf, r->bytes) : 0);
}

struct mux_ctx
{
	struct mpeg4_avc_t avc;
	uint8_t extra[512];
	int extra_n;
};

static void mux_prepare(struct mux_ctx* m)
{
	uint8_t au[512], tmp[1024];
	int vcl = 0;
	memset(m, 0, sizeof(*m));
	to_avcc(&m->avc, tmp, sizeof(tmp), au, make_au(au, 0), &vcl);
	m->extra_n = mpeg4_avc_decoder_configuration_record_save(&m->avc, m->extra, sizeof(m->extra));
}

static void kat_mov(void)
{
	struct mux_ctx m;
	struct gbuf g = { NULL, 0, 0, 0 };
	struct gbuf g2;
	struct rd r;
	struct mov_reader_trackinfo_t ti = { mov_video, mov_audio, mov_sub };
	mov_writer_t* w;
	fmp4_writer_t* fw;
	mov_reader_t* rdr;
	uint8_t au[1024], conv[2048], aac[256];
	int i, vt, at, vcl, rc, n;
	int64_t ts;

	mux_prepare(&m);
	w = mov_writer_create(&kMovIo, &g, 0);
	vt = mov_writer_add_video(w, MOV_OBJECT_H264, 320, 180, m.extra, (size_t)m.extra_n);
	at = mov_writer_add_audio(w, MOV_OBJECT_AAC, 2, 16, 44100, kAsc, sizeof(kAsc));
	for (i = 0; i < NFRAMES; i++)
	{
		size_t n2 = to_avcc(&m.avc, conv, sizeof(conv), au, make_au(au, i), &vcl);
		mov_writer_write(w, vt, conv, n2, vts(i), vts(i), 0 == i ? MOV_AV_FLAG_KEYFREAME : 0);
	}
	for (i = 0; i < NAUDIO; i++)
		mov_writer_write(w, at, aac, make_aac(aac, i), ats(i), ats(i), 0);
	mov_writer_destroy(w);
	printf("mov.file: bytes=%llu\n", (unsigned long long)g.n); // header timestamps differ per run: no CRC
	emit("mov.mp4", g.p, (size_t)g.n);

	memset(&r, 0, sizeof(r));
	r.cap = 1 << 16;
	r.buf = (uint8_t*)malloc(r.cap);
	g.off = 0;
	rdr = mov_reader_create(&kMovIo, &g);
	rc = mov_reader_getinfo(rdr, &ti, &r);
	printf("mov.getinfo: rc=%d duration=%llu\n", rc, (unsigned long long)mov_reader_getduration(rdr));
	print_info("mov", &r);
	n = 0;
	while ((rc = mov_reader_read2(rdr, rd_alloc, &r)) > 0)
		print_sample("mov", n++, rc, &r);
	printf("mov.read_end: rc=%d samples=%d\n", rc, n);
	ts = 80;
	rc = mov_reader_seek(rdr, &ts);
	printf("mov.seek: rc=%d ts=%lld\n", rc, (long long)ts);
	rc = mov_reader_read2(rdr, rd_alloc, &r);
	print_sample("mov.afterseek", 0, rc, &r);
	mov_reader_destroy(rdr);

	memset(&g2, 0, sizeof(g2));
	fw = fmp4_writer_create(&kMovIo, &g2, 0);
	vt = fmp4_writer_add_video(fw, MOV_OBJECT_H264, 320, 180, m.extra, (size_t)m.extra_n);
	at = fmp4_writer_add_audio(fw, MOV_OBJECT_AAC, 2, 16, 44100, kAsc, sizeof(kAsc));
	fmp4_writer_init_segment(fw);
	for (i = 0; i < NFRAMES; i++)
	{
		size_t n2 = to_avcc(&m.avc, conv, sizeof(conv), au, make_au(au, i), &vcl);
		fmp4_writer_write(fw, vt, conv, n2, vts(i), vts(i), 0 == i ? MOV_AV_FLAG_KEYFREAME : 0);
		if (i < NAUDIO)
			fmp4_writer_write(fw, at, aac, make_aac(aac, i), ats(i), ats(i), 0);
	}
	fmp4_writer_save_segment(fw);
	fmp4_writer_destroy(fw);
	printf("fmp4.file: bytes=%llu\n", (unsigned long long)g2.n);
	emit("fmp4.mp4", g2.p, (size_t)g2.n);

	memset(&r, 0, sizeof(r));
	r.cap = 1 << 16;
	r.buf = (uint8_t*)malloc(r.cap);
	g2.off = 0;
	rdr = mov_reader_create(&kMovIo, &g2);
	rc = mov_reader_getinfo(rdr, &ti, &r);
	printf("fmp4.getinfo: rc=%d\n", rc);
	print_info("fmp4", &r);
	n = 0;
	while ((rc = mov_reader_read2(rdr, rd_alloc, &r)) > 0)
		print_sample("fmp4", n++, rc, &r);
	printf("fmp4.read_end: rc=%d samples=%d\n", rc, n);
	mov_reader_destroy(rdr);
	free(r.buf);
	free(g.p);
	free(g2.p);
}

static void kat_mkv(void)
{
	struct mux_ctx m;
	struct gbuf g = { NULL, 0, 0, 0 };
	struct rd r;
	struct mkv_reader_trackinfo_t ti = { mkv_video, mkv_audio, mkv_sub };
	mkv_writer_t* w;
	mkv_reader_t* rdr;
	uint8_t au[1024], conv[2048], aac[256];
	int i, vt, at, vcl, rc, n;

	mux_prepare(&m);
	w = mkv_writer_create(&kMkvIo, &g, 0);
	vt = mkv_writer_add_video(w, MKV_CODEC_VIDEO_H264, 320, 180, m.extra, (size_t)m.extra_n);
	at = mkv_writer_add_audio(w, MKV_CODEC_AUDIO_AAC, 2, 16, 44100, kAsc, sizeof(kAsc));
	for (i = 0; i < NFRAMES; i++)
	{
		size_t n2 = to_avcc(&m.avc, conv, sizeof(conv), au, make_au(au, i), &vcl);
		mkv_writer_write(w, vt, conv, n2, vts(i), vts(i), 0 == i ? MKV_FLAGS_KEYFRAME : 0);
	}
	for (i = 0; i < NAUDIO; i++)
		mkv_writer_write(w, at, aac, make_aac(aac, i), ats(i), ats(i), MKV_FLAGS_KEYFRAME);
	mkv_writer_destroy(w);
	printf("mkv.file: bytes=%llu\n", (unsigned long long)g.n); // random SegmentUID: no CRC
	emit("mkv.mkv", g.p, (size_t)g.n);

	memset(&r, 0, sizeof(r));
	r.cap = 1 << 16;
	r.buf = (uint8_t*)malloc(r.cap);
	g.off = 0;
	rdr = mkv_reader_create(&kMkvIo, &g);
	rc = mkv_reader_getinfo(rdr, &ti, &r);
	printf("mkv.getinfo: rc=%d duration=%llu\n", rc, (unsigned long long)mkv_reader_getduration(rdr));
	print_info("mkv", &r);
	n = 0;
	while ((rc = mkv_reader_read2(rdr, rd_alloc, &r)) > 0)
		print_sample("mkv", n++, rc, &r);
	printf("mkv.read_end: rc=%d samples=%d\n", rc, n);
	mkv_reader_destroy(rdr);
	free(r.buf);
	free(g.p);
}

// ---- 3. FLV ------------------------------------------------------------------------------------------------------

struct flvmux
{
	struct gbuf file;   // FLV file: header + tags
	struct gbuf tags;   // [type][len:2][body] records, the flv_demuxer fuzz input
	uint32_t prev;
	int ntags;
};

static void put24(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }

static int flv_on_tag(void* param, int type, const void* data, size_t bytes, uint32_t timestamp)
{
	struct flvmux* f = (struct flvmux*)param;
	uint8_t th[11], rec[3], pt[4];
	th[0] = (uint8_t)type;
	put24(th + 1, (uint32_t)bytes);
	put24(th + 4, timestamp & 0xFFFFFF);
	th[7] = (uint8_t)(timestamp >> 24);
	put24(th + 8, 0);
	gb_write(&f->file, th, sizeof(th));
	gb_write(&f->file, data, bytes);
	pt[0] = (uint8_t)((bytes + 11) >> 24); pt[1] = (uint8_t)((bytes + 11) >> 16); pt[2] = (uint8_t)((bytes + 11) >> 8); pt[3] = (uint8_t)(bytes + 11);
	gb_write(&f->file, pt, 4);
	rec[0] = (uint8_t)(type == 8 ? 0 : type == 9 ? 1 : 2);
	rec[1] = (uint8_t)(bytes >> 8);
	rec[2] = (uint8_t)bytes;
	gb_write(&f->tags, rec, 3);
	gb_write(&f->tags, data, bytes);
	f->ntags++;
	return 0;
}

static int flv_on_packet(void* param, int codec, const void* data, size_t bytes, uint32_t pts, uint32_t dts, int flags)
{
	int* n = (int*)param;
	printf("flv.pkt.%d: codec=%d bytes=%zu pts=%u dts=%u flags=%d crc=%08x\n", (*n)++, codec, bytes, pts, dts, flags, crc32(data, bytes));
	return 0;
}

static void* flv_alloc(void* param, size_t bytes) { (void)param; return malloc(bytes); }
static void flv_free(void* param, void* ptr) { (void)param; free(ptr); }

static void kat_flv(void)
{
	static const uint8_t hdr[] = { 'F', 'L', 'V', 1, 5, 0, 0, 0, 9, 0, 0, 0, 0 };
	struct flvmux f;
	flv_muxer_t* mux;
	flv_demuxer_t* demux;
	struct flv_parser_t parser;
	uint8_t au[1024], adts[256];
	int i, n = 0, r;
	size_t off;
	int step;

	memset(&f, 0, sizeof(f));
	gb_write(&f.file, hdr, sizeof(hdr));
	mux = flv_muxer_create(flv_on_tag, &f);
	for (i = 0; i < NFRAMES; i++)
	{
		r = flv_muxer_avc(mux, au, make_au(au, i), (uint32_t)vts(i), (uint32_t)vts(i));
		if (r != 0) printf("flv.mux_avc_failed: %d\n", r);
		if (i < NAUDIO)
		{
			r = flv_muxer_aac(mux, adts, make_adts(adts, i), (uint32_t)ats(i), (uint32_t)ats(i));
			if (r != 0) printf("flv.mux_aac_failed: %d\n", r);
		}
	}
	flv_muxer_destroy(mux);
	printf("flv.mux: tags=%d file_bytes=%llu file_crc=%08x\n", f.ntags, (unsigned long long)f.file.n, crc32(f.file.p, (size_t)f.file.n));
	emit("flv.flv", f.file.p, (size_t)f.file.n);
	emit("flv-tags.bin", f.tags.p, (size_t)f.tags.n);

	// stream parser, whole and trickled
	for (step = 0; step < 2; step++)
	{
		size_t chunk = step ? 5 : (size_t)f.file.n;
		n = 0;
		memset(&parser, 0, sizeof(parser));
		parser.alloc = flv_alloc;
		parser.free = flv_free;
		r = 0;
		for (off = 0; off < f.file.n && r >= 0; off += chunk)
			r = flv_parser_input(&parser, f.file.p + off, f.file.n - off < chunk ? (size_t)f.file.n - off : chunk, flv_on_packet, &n);
		printf("flv.parser.%s: r=%d packets=%d\n", step ? "trickle" : "whole", r, n);
	}
	free(f.file.p);
	free(f.tags.p);

	// tag demuxer fed directly by a second muxer pass
	{
		struct flvmux f2;
		const uint8_t* p;
		memset(&f2, 0, sizeof(f2));
		mux = flv_muxer_create(flv_on_tag, &f2);
		for (i = 0; i < NFRAMES; i++)
			flv_muxer_avc(mux, au, make_au(au, i), (uint32_t)vts(i), (uint32_t)vts(i));
		flv_muxer_destroy(mux);
		n = 0;
		demux = flv_demuxer_create(flv_on_packet, &n);
		for (p = f2.tags.p; p < f2.tags.p + f2.tags.n;)
		{
			size_t len = ((size_t)p[1] << 8) | p[2];
			static const int types[3] = { 8, 9, 18 };
			r = flv_demuxer_input(demux, types[p[0] % 3], p + 3, len, (uint32_t)(p - f2.tags.p));
			if (r < 0)
				break;
			p += 3 + len;
		}
		flv_demuxer_destroy(demux);
		printf("flv.demuxer: r=%d packets=%d\n", r, n);
		free(f2.file.p);
		free(f2.tags.p);
	}
}

// ---- 4. MPEG-TS / MPEG-PS ----------------------------------------------------------------------------------------

static void* mp_alloc(void* param, size_t bytes) { (void)param; return malloc(bytes); }
static void mp_free(void* param, void* p) { (void)param; free(p); }
static int ts_write(void* param, const void* packet, size_t bytes) { return gb_write(param, packet, bytes); }
static int ps_write(void* param, int stream, void* packet, size_t bytes) { (void)stream; return gb_write(param, packet, bytes); }

static int ts_on_packet(void* param, int program, int stream, int codecid, int flags, int64_t pts, int64_t dts, const void* data, size_t bytes)
{
	int* n = (int*)param;
	printf("ts.pkt.%d: program=%d stream=%d codec=%d flags=%d pts=%lld dts=%lld bytes=%zu crc=%08x\n", (*n)++, program, stream, codecid, flags,
		(long long)pts, (long long)dts, bytes, crc32(data, bytes));
	return 0;
}

static int ps_on_packet(void* param, int stream, int codecid, int flags, int64_t pts, int64_t dts, const void* data, size_t bytes)
{
	int* n = (int*)param;
	printf("ps.pkt.%d: stream=%d codec=%d flags=%d pts=%lld dts=%lld bytes=%zu crc=%08x\n", (*n)++, stream, codecid, flags,
		(long long)pts, (long long)dts, bytes, crc32(data, bytes));
	return 0;
}

static void kat_ts(void)
{
	struct mpeg_ts_func_t fn = { mp_alloc, mp_free, ts_write };
	struct gbuf g = { NULL, 0, 0, 0 };
	struct ts_demuxer_t* d;
	void* ts;
	uint8_t au[1024], adts[256];
	int i, vs, as, n = 0, r = 0;
	size_t off;

	ts = mpeg_ts_create(&fn, &g);
	vs = mpeg_ts_add_stream(ts, PSI_STREAM_H264, NULL, 0);
	as = mpeg_ts_add_stream(ts, PSI_STREAM_AAC, NULL, 0);
	for (i = 0; i < NFRAMES; i++)
	{
		r |= mpeg_ts_write(ts, vs, 0 == i ? 1 : 0, (int64_t)vts(i) * 90 + 900, (int64_t)vts(i) * 90 + 900, au, make_au(au, i));
		if (i < NAUDIO)
			r |= mpeg_ts_write(ts, as, 0, (int64_t)ats(i) * 90 + 900, (int64_t)ats(i) * 90 + 900, adts, make_adts(adts, i));
	}
	mpeg_ts_destroy(ts);
	printf("ts.mux: r=%d streams=%d,%d bytes=%llu packets188=%llu crc=%08x\n", r, vs, as, (unsigned long long)g.n, (unsigned long long)(g.n / 188), crc32(g.p, (size_t)g.n));
	emit("mux.ts", g.p, (size_t)g.n);

	d = ts_demuxer_create(ts_on_packet, &n);
	for (off = 0, r = 0; off + 188 <= g.n && r >= 0; off += 188)
		r = ts_demuxer_input(d, g.p + off, 188);
	ts_demuxer_flush(d);
	ts_demuxer_destroy(d);
	printf("ts.demux: r=%d packets=%d\n", r, n);
	free(g.p);
}

static void kat_ps(void)
{
	struct ps_muxer_func_t fn = { mp_alloc, mp_free, ps_write };
	struct gbuf g = { NULL, 0, 0, 0 };
	struct ps_muxer_t* ps;
	struct ps_demuxer_t* d;
	uint8_t au[1024], adts[256];
	int i, vs, as, n = 0, r = 0, k;
	size_t off = 0;

	ps = ps_muxer_create(&fn, &g);
	vs = ps_muxer_add_stream(ps, PSI_STREAM_H264, NULL, 0);
	as = ps_muxer_add_stream(ps, PSI_STREAM_AAC, NULL, 0);
	for (i = 0; i < NFRAMES; i++)
	{
		r |= ps_muxer_input(ps, vs, 0 == i ? 1 : 0, (int64_t)vts(i) * 90 + 900, (int64_t)vts(i) * 90 + 900, au, make_au(au, i));
		if (i < NAUDIO)
			r |= ps_muxer_input(ps, as, 0, (int64_t)ats(i) * 90 + 900, (int64_t)ats(i) * 90 + 900, adts, make_adts(adts, i));
	}
	ps_muxer_destroy(ps);
	printf("ps.mux: r=%d streams=%d,%d bytes=%llu crc=%08x\n", r, vs, as, (unsigned long long)g.n, crc32(g.p, (size_t)g.n));
	emit("mux.ps", g.p, (size_t)g.n);

	d = ps_demuxer_create(ps_on_packet, &n);
	while (off < g.n && (k = ps_demuxer_input(d, g.p + off, (size_t)g.n - off)) > 0)
		off += (size_t)k;
	ps_demuxer_destroy(d);
	printf("ps.demux: consumed=%zu of %llu packets=%d\n", off, (unsigned long long)g.n, n);
	free(g.p);
}

// ---- 5. RTP ------------------------------------------------------------------------------------------------------

struct rtpctx
{
	struct gbuf records; // [len:2][packet]
	int npackets;
	uint32_t first_header_crc;
	int nframes;
};

static void* rtp_alloc(void* param, int bytes) { (void)param; return malloc((size_t)bytes + 4); }
static void rtp_free(void* param, void* p) { (void)param; free(p); }

static int rtp_enc_packet(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
	struct rtpctx* c = (struct rtpctx*)param;
	uint8_t len[2];
	(void)timestamp; (void)flags;
	len[0] = (uint8_t)((unsigned)bytes >> 8);
	len[1] = (uint8_t)bytes;
	gb_write(&c->records, len, 2);
	gb_write(&c->records, packet, (size_t)bytes);
	if (0 == c->npackets)
		c->first_header_crc = crc32(packet, 12);
	c->npackets++;
	return 0;
}

static int rtp_dec_packet(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
	struct rtpctx* c = (struct rtpctx*)param;
	printf("rtp.frame.%d: bytes=%d ts=%u flags=0x%x crc=%08x\n", c->nframes++, bytes, timestamp, flags, crc32(packet, (size_t)bytes));
	return 0;
}

static void rtp_roundtrip(const char* label, int payload, const char* name, uint32_t freq_step, int seedbase, const char* seedfile, int h264)
{
	struct rtp_payload_t enc = { rtp_alloc, rtp_free, rtp_enc_packet };
	struct rtp_payload_t dec = { rtp_alloc, rtp_free, rtp_dec_packet };
	struct rtpctx c;
	void *e, *d;
	uint8_t au[1024];
	int i, r = 0;
	const uint8_t* p;
	uint8_t selector;

	memset(&c, 0, sizeof(c));
	rtp_packet_setsize(120);
	e = rtp_payload_encode_create(payload, name, 0x1234, 0xCAFEBABE, &enc, &c);
	if (!e)
	{
		printf("rtp.%s.encode_create: failed\n", label);
		return;
	}
	for (i = 0; i < 4; i++)
	{
		size_t n = h264 ? make_au(au, i) : pat(au, seedbase + i, 160);
		const uint8_t* data = au;
		if (h264)
		{
			// RTP H.264 payloader takes Annex-B; the same AU is the golden input
		}
		r |= rtp_payload_encode_input(e, data, (int)n, freq_step * (uint32_t)i);
	}
	rtp_payload_encode_destroy(e);
	printf("rtp.%s.encode: r=%d packets=%d header0_crc=%08x records_crc=%08x\n", label, r, c.npackets, c.first_header_crc, crc32(c.records.p, (size_t)c.records.n));

	{
		uint8_t* out = (uint8_t*)malloc((size_t)c.records.n + 1);
		selector = (uint8_t)(h264 ? 0 : 13);
		out[0] = selector;
		memcpy(out + 1, c.records.p, (size_t)c.records.n);
		emit(seedfile, out, (size_t)c.records.n + 1);
		free(out);
	}

	c.nframes = 0;
	d = rtp_payload_decode_create(payload, name, &dec, &c);
	for (p = c.records.p; p < c.records.p + c.records.n;)
	{
		size_t len = ((size_t)p[0] << 8) | p[1];
		r = rtp_payload_decode_input(d, p + 2, (int)len);
		if (r < 0)
		{
			printf("rtp.%s.decode_error: r=%d\n", label, r);
			break;
		}
		p += 2 + len;
	}
	rtp_payload_decode_destroy(d);
	for (i = 0; i < 4; i++)
	{
		size_t n = h264 ? make_au(au, i) : pat(au, seedbase + i, 160);
		if (h264)
			printf("rtp.%s.expect.%d: bytes=%zu crc=%08x\n", label, i, n, crc32(au + 4, n - 4)); // decoder output has no start code of its own
		else
			printf("rtp.%s.expect.%d: bytes=%zu crc=%08x\n", label, i, n, crc32(au, n));
	}
	free(c.records.p);
}

static void kat_rtp(void)
{
	rtp_roundtrip("h264", 96, "H264", 3600, 0, "rtp-h264.bin", 1);
	rtp_roundtrip("pcmu", 0, "PCMU", 160, 50, "rtp-pcmu.bin", 0);
}

// ---- 6. RTP receive path: header parse + jitter buffer -----------------------------------------------------------

static int dm_packet(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
	int* n = (int*)param;
	printf("rtp.demux.pkt.%d: bytes=%d ts=%u flags=0x%x crc=%08x\n", (*n)++, bytes, timestamp, flags, crc32(packet, (size_t)bytes));
	return 0;
}

static void kat_rtp_demux(void)
{
	// three PCMU packets with seq 7,8,9, delivered to the jitter buffer in order; it hands them out with the
	// 12-byte header stripped
	uint8_t pkt[3][12 + 20];
	uint8_t seeds[1 + 3 * (2 + 32) + 2 + 28];
	struct rtp_demuxer_t* d;
	int i, j, n = 0, r, order[3] = { 0, 2, 1 };
	size_t so = 0;
	int lost, late, misorder, duplicate;
	static const uint8_t sr[28] = { 0x80, 200, 0, 6, 0xDE, 0xAD, 0xBE, 0xEF, 0xE0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1F, 0x40, 0, 0, 0, 3, 0, 0, 0x01, 0x80 };

	seeds[so++] = 0;
	for (i = 0; i < 3; i++)
	{
		memset(pkt[i], 0, sizeof(pkt[i]));
		pkt[i][0] = 0x80;
		pkt[i][1] = 0; // PCMU
		pkt[i][2] = 0; pkt[i][3] = (uint8_t)(7 + i);
		pkt[i][4] = 0; pkt[i][5] = 0; pkt[i][6] = (uint8_t)(((i * 160) >> 8) & 0xFF); pkt[i][7] = (uint8_t)((i * 160) & 0xFF);
		pkt[i][8] = 0xAA; pkt[i][9] = 0xBB; pkt[i][10] = 0xCC; pkt[i][11] = 0xDD;
		for (j = 0; j < 20; j++)
			pkt[i][12 + j] = (uint8_t)(i * 20 + j + 1);
	}
	for (i = 0; i < 3; i++)
	{
		seeds[so++] = 0; seeds[so++] = 32;
		memcpy(seeds + so, pkt[order[i]], 32);
		so += 32;
	}
	seeds[so++] = 0; seeds[so++] = 28;
	memcpy(seeds + so, sr, 28);
	so += 28;
	emit("rtp-demux.bin", seeds, so);

	d = rtp_demuxer_create(0, 8000, 0, "PCMU", dm_packet, &n);
	for (i = 0; i < 3; i++)
	{
		r = rtp_demuxer_input(d, pkt[i], 32); // in order: the reorder path depends on wall-clock jitter timing
		printf("rtp.demux.input.%d: r=%d\n", i, r);
	}
	r = rtp_demuxer_input(d, sr, 28);
	printf("rtp.demux.rtcp: r=%d\n", r);
	rtp_demuxer_stats(d, &lost, &late, &misorder, &duplicate);
	printf("rtp.demux.stats: delivered=%d lost=%d late=%d misorder=%d duplicate=%d\n", n, lost, late, misorder, duplicate);
	rtp_demuxer_destroy(&d);
}

int main(int argc, char** argv)
{
	g_dir = argc > 1 ? argv[1] : NULL;
	setvbuf(stdout, NULL, _IOLBF, 0);
	kat_config();
	kat_mov();
	kat_mkv();
	kat_flv();
	kat_ts();
	kat_ps();
	kat_rtp();
	kat_rtp_demux();
	return 0;
}
