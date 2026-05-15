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

#include <atomic>
#include <cstddef>
#include <vector>

/**
 * Ringbuffer (FIFO) Implementation
 *
 * @ingroup audio
 */
class cSoftHdRingbuffer {
public:
	cSoftHdRingbuffer(size_t);

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
	std::vector<char> m_buffer;   ///< ring buffer data
	char *m_pBuffer;              ///< pointer ring buffer data

	size_t m_size;                ///< bytes in buffer (for faster calc)

	const char *m_pBufferEnd;     ///< end of buffer
	const char *m_pReadPointer;   ///< only used by reader
	char *m_pWritePointer;        ///< only used by writer

	// The only thing modified by both
	std::atomic<size_t> m_filled; ///< how many of the buffer is used
};

#endif
