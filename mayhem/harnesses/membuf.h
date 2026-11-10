#ifndef MAYHEM_MEMBUF_H
#define MAYHEM_MEMBUF_H
#include <errno.h>
#include <stdint.h>
#include <string.h>

/* Read-only in-memory io for the mov/mkv readers (their buffer callbacks: read/write/seek/tell). */
struct membuf
{
	const uint8_t* ptr;
	uint64_t bytes;
	uint64_t off;
};

static int membuf_read(void* param, void* data, uint64_t bytes)
{
	struct membuf* m = (struct membuf*)param;
	if (bytes > m->bytes - m->off)
		return -E2BIG;
	memcpy(data, m->ptr + m->off, (size_t)bytes);
	m->off += bytes;
	return 0;
}

static int membuf_write(void* param, const void* data, uint64_t bytes)
{
	(void)param; (void)data; (void)bytes;
	return -EPERM;
}

/* offset >= 0 seeks from the beginning, offset < 0 from the end (the io contract of both readers). */
static int membuf_seek(void* param, int64_t offset)
{
	struct membuf* m = (struct membuf*)param;
	if (offset >= 0)
	{
		if ((uint64_t)offset > m->bytes)
			return -E2BIG;
		m->off = (uint64_t)offset;
	}
	else
	{
		uint64_t n = (uint64_t)0 - (uint64_t)offset; // unsigned magnitude: -INT64_MIN overflows in signed arithmetic
		if (n > m->bytes)
			return -E2BIG;
		m->off = m->bytes - n;
	}
	return 0;
}

static int64_t membuf_tell(void* param)
{
	return (int64_t)((struct membuf*)param)->off;
}
#endif
