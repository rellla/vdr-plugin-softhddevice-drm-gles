// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file ringbuffer.h
 * Audio Ringbuffer Header File
 *
 * @copyright 2009, 2011, 2014 by Johns. All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __SOFTHDRINGBUFFER_H
#define __SOFTHDRINGBUFFER_H

#include <cstddef>

/**
 * @addtogroup audio
 * @{
 */

/**
 * Atomic Wrapper Macros
 */
typedef volatile int atomic_t;  ///< atomic type, 24 bit useable

#define atomic_set(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_read(ptr) __atomic_load_n(ptr, __ATOMIC_SEQ_CST)
#define atomic_inc(ptr) __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST)
#define atomic_dec(ptr) __atomic_sub_fetch(ptr, 1, __ATOMIC_SEQ_CST)
#define atomic_add(val, ptr) __atomic_add_fetch(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_sub(val, ptr) __atomic_sub_fetch(ptr, val, __ATOMIC_SEQ_CST)

/** @} */

/**
 * Ringbuffer (FIFO) Implementation
 *
 * @ingroup audio
 */
class cSoftHdRingbuffer {
public:
	cSoftHdRingbuffer(size_t);
	~cSoftHdRingbuffer(void);
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
	size_t m_size;                ///< bytes in buffer (for faster calc)
	const char *m_pReadPointer;   ///< only used by reader
	char *m_pWritePointer;        ///< only used by writer

	// The only thing modified by both
	atomic_t m_filled;            ///< how many of the buffer is used
};

#endif
