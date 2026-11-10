#ifndef MAYHEM_RECORDS_H
#define MAYHEM_RECORDS_H
#include <stddef.h>
#include <stdint.h>

/* Splits a fuzz input into [len:2 big-endian][payload] records, so one input can carry a
 * multi-packet / multi-tag stream into a stateful parser. A record whose length runs past
 * the end of the input is truncated to what is left. */
static inline int next_record(const uint8_t** data, size_t* size, const uint8_t** rec, size_t* rec_len)
{
	size_t n;
	if (*size < 2)
		return 0;
	n = ((size_t)(*data)[0] << 8) | (*data)[1];
	*data += 2;
	*size -= 2;
	if (n > *size)
		n = *size;
	*rec = *data;
	*rec_len = n;
	*data += n;
	*size -= n;
	return 1;
}

/* Reads every byte of a buffer a parser handed to a callback, so ASan checks the claimed length. */
static inline unsigned touch(const void* p, size_t n)
{
	volatile unsigned sum = 0;
	const volatile uint8_t* b = (const volatile uint8_t*)p;
	size_t i;
	for (i = 0; i < n; i++)
		sum += b[i];
	return sum;
}
#endif
