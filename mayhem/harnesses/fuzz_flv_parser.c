// Fuzzes the streaming FLV file parser (libflv flv_parser_input): header, tag headers, audio/video/script
// tag bodies, AMF0 metadata and the codec config records inside them. The same bytes go in twice: in one
// piece and trickled in 7-byte chunks, which exercises the parser's partial-header state machine.
#include "flv-parser.h"
#include "records.h"
#include <stdlib.h>
#include <string.h>

struct ctx
{
	void* body; // the single outstanding tag-body allocation, so a truncated stream does not leak it
};

static const size_t kMaxBody = 16u << 20;

// A tag body is at most 24 bits on the wire; anything bigger comes from the parser's own size arithmetic
// (libflv flv-parser.c:211 underflows `tag.size - expect`), so refuse it like a failed malloc: the parser returns -1.
static void* on_alloc(void* param, size_t bytes)
{
	struct ctx* c = (struct ctx*)param;
	if (bytes > kMaxBody)
		return NULL;
	free(c->body);
	c->body = malloc(bytes ? bytes : 1);
	return c->body;
}

static void on_free(void* param, void* ptr)
{
	struct ctx* c = (struct ctx*)param;
	free(ptr);
	c->body = NULL;
}

static int on_packet(void* param, int codec, const void* data, size_t bytes, uint32_t pts, uint32_t dts, int flags)
{
	(void)param; (void)codec; (void)pts; (void)dts; (void)flags;
	touch(data, bytes);
	return 0;
}

static void run(const uint8_t* data, size_t size, size_t step)
{
	struct ctx c = { NULL };
	struct flv_parser_t parser;
	size_t off;

	memset(&parser, 0, sizeof(parser));
	parser.alloc = on_alloc;
	parser.free = on_free;
	for (off = 0; off < size; off += step)
	{
		size_t n = size - off < step ? size - off : step;
		if (flv_parser_input(&parser, data + off, n, on_packet, &c) < 0)
			break;
	}
	free(c.body);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	if (size < 9) // an FLV header is 9 bytes
		return -1;
	run(data, size, size);
	run(data, size, 7);
	return 0;
}
