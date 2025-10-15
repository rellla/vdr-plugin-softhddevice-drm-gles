/**
 * @file ringbuffer.cpp
 * Ringbuffer class
 *
 * This file defines cSoftHdRinguffer, which is a ringbuffer
 * implementation used for the audio data.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iatomic.h"
#include "ringbuffer.h"

#include "logger.h"

/******************************************************************************
 * cSoftHdRingbuffer class
 *
 * Lock free ring buffer with only one writer and one reader
 *****************************************************************************/

/**
 * cSoftHdRingbuffer constructor
 *
 * Init a new ring buffer
 *
 * @param size    Size of the ring buffer
 */
cSoftHdRingbuffer::cSoftHdRingbuffer(size_t size)
{
	if (!(m_pBuffer = (char *)malloc(size)))	// allocate buffer
	LOGFATAL("ringbuffer: %s: can't allocate memory for ringbuffer", __FUNCTION__);

	m_Size = size;
	m_pBufferEnd = m_pBuffer + size;
	m_pReadPointer = m_pBuffer;
	m_pWritePointer = m_pBuffer;
	atomic_set(&m_filled, 0);
}

/**
 * cSoftHdRingbuffer destructor
 */
cSoftHdRingbuffer::~cSoftHdRingbuffer(void)
{
	free(m_pBuffer);
}

/**
 * Reset ring buffer pointers
 */
void cSoftHdRingbuffer::Reset(void)
{
	m_pReadPointer = m_pBuffer;
	m_pWritePointer = m_pBuffer;
	atomic_set(&m_filled, 0);
}

/**
 * Advance write pointer in ring buffer
 *
 * @param cnt        Number of bytes to be adavanced
 *
 * @returns          Number of bytes that could be advanced in ring buffer
 */
size_t cSoftHdRingbuffer::WriteAdvance(size_t cnt)
{
	size_t n;

	n = m_Size - atomic_read(&m_filled);
	if (cnt > n) {		// not enough space
		cnt = n;
	}
	//
	//	Hitting end of buffer?
	//
	n = m_pBufferEnd - m_pWritePointer;
	if (n > cnt) {		// don't cross the end
		m_pWritePointer += cnt;
	} else {		// reached or cross the end
		m_pWritePointer = m_pBuffer;
		if (n < cnt) {
			n = cnt - n;
			m_pWritePointer += n;
		}
	}

	//
	//	Only atomic modification!
	//
	atomic_add(cnt, &m_filled);
	return cnt;
}

/**
 * Write to a ring buffer
 *
 * @param buf   buffer of @p cnt bytes to be written
 * @param cnt   Number of bytes in buffer
 *
 * @returns     The number of bytes that could be placed in the ring buffer
 */
size_t cSoftHdRingbuffer::Write(const void *buf, size_t cnt)
{
	size_t n;

	n = m_Size - atomic_read(&m_filled);
	if (cnt > n) {			// not enough space
		cnt = n;
	}
	//
	//	Hitting end of buffer?
	//
	n = m_pBufferEnd - m_pWritePointer;
	if (n > cnt) {			// don't cross the end
		memcpy(m_pWritePointer, buf, cnt);
		m_pWritePointer += cnt;
	} else {				// reached or cross the end
		memcpy(m_pWritePointer, buf, n);
		m_pWritePointer = m_pBuffer;
		if (n < cnt) {
			buf = (uint8_t *)buf + n;
			n = cnt - n;
			memcpy(m_pWritePointer, buf, n);
			m_pWritePointer += n;
		}
	}

	//
	//	Only atomic modification!
	//
	atomic_add(cnt, &m_filled);
	return cnt;
}

/**
 * Get write pointer and free bytes at this position of ring buffer
 *
 * @param[out] wp         Write pointer is placed here
 *
 * @returns               The number of bytes that could be placed in the ring
 *                        buffer at the write pointer.
 */
size_t cSoftHdRingbuffer::GetWritePointer(void **wp)
{
	size_t n;
	size_t cnt;

	//	Total free bytes available in ring buffer
	cnt = m_Size - atomic_read(&m_filled);

	*wp = m_pWritePointer;

	//
	//	Hitting end of buffer?
	//
	n = m_pBufferEnd - m_pWritePointer;
	if (n <= cnt) {			// reached or cross the end
		return n;
	}
	return cnt;
}

/**
 * Advance read pointer in ring buffer
 *
 * @param cnt       Number of bytes to be advanced
 *
 * @returns         Number of bytes that could be advanced in ring buffer
 */
size_t cSoftHdRingbuffer::ReadAdvance(size_t cnt)
{
	size_t n;

	n = atomic_read(&m_filled);
	if (cnt > n) {			// not enough filled
		cnt = n;
	}
	//
	//	Hitting end of buffer?
	//
	n = m_pBufferEnd - m_pReadPointer;
	if (n > cnt) {			// don't cross the end
		m_pReadPointer += cnt;
	} else {				// reached or cross the end
		m_pReadPointer = m_pBuffer;
		if (n < cnt) {
			n = cnt - n;
			m_pReadPointer += n;
		}
	}

	//
	//	Only atomic modification!
	//
	atomic_sub(cnt, &m_filled);
	return cnt;
}

/**
 * Read from a ring buffer.
 *
 * @param buf   buffer of @p cnt bytes to be read
 * @param cnt   Number of bytes to be read
 *
 * @returns     Number of bytes that could be read from ring buffer
 */
size_t cSoftHdRingbuffer::Read(void *buf, size_t cnt)
{
	size_t n;

	n = atomic_read(&m_filled);
	if (cnt > n) {			// not enough filled
		cnt = n;
	}
	//
	//	Hitting end of buffer?
	//
	n = m_pBufferEnd - m_pReadPointer;
	if (n > cnt) {			// don't cross the end
		memcpy(buf, m_pReadPointer, cnt);
		m_pReadPointer += cnt;
	} else {				// reached or cross the end
		memcpy(buf, m_pReadPointer, n);
		m_pReadPointer = m_pBuffer;
		if (n < cnt) {
			buf = (uint8_t *)buf + n;
			n = cnt - n;
			memcpy(buf, m_pReadPointer, n);
			m_pReadPointer += n;
		}
	}

	//
	//	Only atomic modification!
	//
	atomic_sub(cnt, &m_filled);
	return cnt;
}

/**
 * Get read pointer and used bytes at this position of ring buffer
 *
 * @param[out] rp    Read pointer is placed here
 *
 * @returns          The number of bytes that could be read from the ring
 *                   buffer at the read pointer
 */
size_t cSoftHdRingbuffer::GetReadPointer(const void **rp)
{
	size_t n;
	size_t cnt;

	//	Total used bytes in ring buffer
	cnt = atomic_read(&m_filled);

	*rp = m_pReadPointer;

	//
	//	Hitting end of buffer?
	//
	n = m_pBufferEnd - m_pReadPointer;
	if (n <= cnt) {			// reached or cross the end
		return n;
	}
	return cnt;
}

/**
 * Get free bytes in ring buffer
 *
 * @returns        Number of bytes free in buffer
 */
size_t cSoftHdRingbuffer::FreeBytes(void)
{
	return m_Size - atomic_read(&m_filled);
}

/**
 * Get used bytes in ring buffer.
 *
 * @returns        Number of bytes used in buffer.
 */
size_t cSoftHdRingbuffer::UsedBytes(void)
{
	return atomic_read(&m_filled);
}
