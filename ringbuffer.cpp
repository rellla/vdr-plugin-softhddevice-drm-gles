// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file ringbuffer.cpp
 * Audio Ringbuffer
 *
 * This file defines cSoftHdRinguffer, which is a ringbuffer
 * implementation used for the audio data.
 *
 * @copyright 2009, 2011, 2014 by Johns. All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "logger.h"
#include "ringbuffer.h"

/**
 * cSoftHdRingbuffer constructor
 *
 * Init a new ring buffer
 *
 * @param size    Size of the ring buffer
 */
cSoftHdRingbuffer::cSoftHdRingbuffer(size_t size)
	: m_buffer(size),
	  m_pBuffer(m_buffer.data()),
	  m_size(size),
	  m_pBufferEnd(m_pBuffer + size),
	  m_pReadPointer(m_pBuffer),
	  m_pWritePointer(m_pBuffer),
	  m_filled(0)
{
}

/**
 * Reset ring buffer pointers
 */
void cSoftHdRingbuffer::Reset(void)
{
	m_pReadPointer = m_pBuffer;
	m_pWritePointer = m_pBuffer;
	m_filled.store(0, std::memory_order_release);
}

/**
 * Advance write pointer in ring buffer
 *
 * @param cnt        Number of bytes to be adavanced
 *
 * @return           Number of bytes that could be advanced in ring buffer
 */
size_t cSoftHdRingbuffer::WriteAdvance(size_t cnt)
{
	size_t n;

	n = m_size - m_filled.load(std::memory_order_acquire);
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
	m_filled.fetch_add(cnt, std::memory_order_release);
	return cnt;
}

/**
 * Write to a ring buffer
 *
 * @param buf   buffer of @p cnt bytes to be written
 * @param cnt   Number of bytes in buffer
 *
 * @return      The number of bytes that could be placed in the ring buffer
 */
size_t cSoftHdRingbuffer::Write(const void *buf, size_t cnt)
{
	size_t n;

	n = m_size - m_filled.load(std::memory_order_acquire);
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
	m_filled.fetch_add(cnt, std::memory_order_release);
	return cnt;
}

/**
 * Get write pointer and free bytes at this position of ring buffer
 *
 * @param[out] wp         Write pointer is placed here
 *
 * @return                The number of bytes that could be placed in the ring
 *                        buffer at the write pointer.
 */
size_t cSoftHdRingbuffer::GetWritePointer(void **wp)
{
	size_t n;
	size_t cnt;

	//	Total free bytes available in ring buffer
	cnt = m_size - m_filled.load(std::memory_order_acquire);

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
 * @return          Number of bytes that could be advanced in ring buffer
 */
size_t cSoftHdRingbuffer::ReadAdvance(size_t cnt)
{
	size_t n;

	n = m_filled.load(std::memory_order_acquire);
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
	m_filled.fetch_sub(cnt, std::memory_order_release);
	return cnt;
}

/**
 * Read from a ring buffer.
 *
 * @param buf   buffer of @p cnt bytes to be read
 * @param cnt   Number of bytes to be read
 *
 * @return      Number of bytes that could be read from ring buffer
 */
size_t cSoftHdRingbuffer::Read(void *buf, size_t cnt)
{
	size_t n;

	n = m_filled.load(std::memory_order_acquire);
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
	m_filled.fetch_sub(cnt, std::memory_order_release);
	return cnt;
}

/**
 * Get read pointer and used bytes at this position of ring buffer
 *
 * @param[out] rp    Read pointer is placed here
 *
 * @return           The number of bytes that could be read from the ring
 *                   buffer at the read pointer
 */
size_t cSoftHdRingbuffer::GetReadPointer(const void **rp)
{
	size_t n;
	size_t cnt;

	//	Total used bytes in ring buffer
	cnt = m_filled.load(std::memory_order_acquire);

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
 * @return         Number of bytes free in buffer
 */
size_t cSoftHdRingbuffer::FreeBytes(void)
{
	return m_size - m_filled.load(std::memory_order_acquire);
}

/**
 * Get used bytes in ring buffer.
 *
 * @return         Number of bytes used in buffer.
 */
size_t cSoftHdRingbuffer::UsedBytes(void)
{
	return m_filled.load(std::memory_order_acquire);
}
