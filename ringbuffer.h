/**
 * @file ringbuffer.h
 * Ringbuffer class header file
 *
 * @copyright (c) 2009, 2011, 2014 by Johns. All Rights Reserved.
 * @copyright (c) 2025 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.}
 */

#ifndef __SOFTHDRINGBUFFER_H
#define __SOFTHDRINGBUFFER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Atomic wrapper macros
 */
typedef volatile int atomic_t;  ///< atomic type, 24 bit useable

#define atomic_set(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_read(ptr) __atomic_load_n(ptr, __ATOMIC_SEQ_CST)
#define atomic_inc(ptr) __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST)
#define atomic_dec(ptr) __atomic_sub_fetch(ptr, 1, __ATOMIC_SEQ_CST)
#define atomic_add(val, ptr) __atomic_add_fetch(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_sub(val, ptr) __atomic_sub_fetch(ptr, val, __ATOMIC_SEQ_CST)

/**
 * cSoftHdRingbuffer - RingBuffer class
 */

class cSoftHdRingbuffer
{
public:
	cSoftHdRingbuffer(size_t);
	virtual ~cSoftHdRingbuffer(void);
	void Reset(void);
	size_t Write(const void *, size_t);
	size_t GetWritePointer(void **);
	size_t WriteAdvance(size_t);
	size_t Read(void *, size_t);
	size_t GetReadPointer(const void **);
	size_t ReadAdvance(size_t);
	size_t FreeBytes(void);
	size_t UsedBytes(void);

private:
	char *m_pBuffer;              ///< ring buffer data
	const char *m_pBufferEnd;     ///< end of buffer
	size_t m_Size;                ///< bytes in buffer (for faster calc)
	const char *m_pReadPointer;   ///< only used by reader
	char *m_pWritePointer;        ///< only used by writer

	// The only thing modified by both
	atomic_t m_filled;            ///< how many of the buffer is used
};
#endif
