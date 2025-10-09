/**
 * @file queue.cpp
 * Thread-safe queue class
 *
 * This file implements cQueue, which is a thread-safe queue
 * implementation.
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

 #include <stdexcept>
 #include <libavutil/frame.h>
 #include <libavcodec/packet.h>

#include "queue.h"
#include "logger.h"

/******************************************************************************
 * cQueue class
 *
 * Thread-safe queue implementation using std::deque and std::mutex.
 * Provides FIFO semantics with bounded capacity.
 *****************************************************************************/

/**
 * Push an element to the front of the queue
 *
 * @tparam T       Element type
 * @param  element The element to push
 * @return true if successfully pushed, false if queue is full
 */
template <typename T>
bool cQueue<T>::Push(const T& element)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_deque.size() >= m_maxSize) {
		return false;
	}

	m_deque.push_front(element);

	return true;
}

/**
 * Pop an element from the back of the queue
 *
 * @tparam T                  Element type
 * @return T                  The popped element
 * @throws std::runtime_error if queue is empty
 */
template <typename T>
T cQueue<T>::Pop(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_deque.empty()) {
		throw std::runtime_error("Queue is empty");
	}

	T element = m_deque.back();
	m_deque.pop_back();

	return element;
}

/**
 * Get a reference to the front element
 *
 * @tparam T                  Element type
 * @return T                  Element at the front (most recently pushed)
 * @throws std::runtime_error if queue is empty
 */
template <typename T>
T cQueue<T>::Head(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_deque.empty()) {
		throw std::runtime_error("Queue is empty");
	}

	return m_deque.front();
}

/**
 * Get a reference to the back element
 *
 * @tparam T                  Element type
 * @return T                  Element at the back (next to be popped)
 * @throws std::runtime_error if queue is empty
 */
template <typename T>
T cQueue<T>::Peek(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_deque.empty()) {
		throw std::runtime_error("Queue is empty");
	}

	return m_deque.back();
}

/**
 * Check if the queue is empty
 *
 * @tparam T Element type
 * @return true if queue is empty, false otherwise
 */
template <typename T>
bool cQueue<T>::Empty(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	return m_deque.empty();
}

/**
 * Get the current size of the queue
 *
 * @tparam T Element type
 * @return Number of elements currently in the queue
 */
template <typename T>
size_t cQueue<T>::Size(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	return m_deque.size();
}

// Explicit template instantiations
template class cQueue<AVFrame *>;   ///< Queue for AVFrame pointers
