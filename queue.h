/**
 * @file queue.h
 * Thread-safe queue header file
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

#ifndef __SOFTHDQUEUE_H
#define __SOFTHDQUEUE_H

#include <cstdint>
#include <deque>
#include <mutex>

/**
 * Thread-safe queue class
 *
 * This class provides a thread-safe queue implementation using std::deque
 * and std::mutex for synchronization. It supports both FIFO queue operations
 * and bounded capacity management.
 *
 * @tparam T The type of elements stored in the queue
 */
template <typename T>
class cQueue
{
public:
	cQueue(size_t maxSize) : m_maxSize(maxSize) {};
	~cQueue(void) {};

	/**
	 * Push an element to the front of the queue
	 *
	 * @param  element            The element to push
	 * @return true if successfully pushed, false if queue is full
	 */
	bool Push(T *element) {
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
	 * @return T                  The popped element, or nullptr if queue is empty
	 */
	T *Pop(void) {
		std::lock_guard<std::mutex> lock(m_mutex);

		if (m_deque.empty()) {
			return nullptr;
		}

		T *element = m_deque.back();
		m_deque.pop_back();

		return element;
	}

	/**
	 * Get a reference to the back element
	 *
	 * @return T                  Element at the back (next to be popped), or nullptr if queue is empty
	 */
	T *Peek(void) {
		std::lock_guard<std::mutex> lock(m_mutex);

		if (m_deque.empty()) {
			return nullptr;
		}

		return m_deque.back();
	}

	/**
	 * Remove all elements from the queue
	 */
	void Clear(void) {
		std::lock_guard<std::mutex> lock(m_mutex);

		m_deque.clear();
	}

	/**
	 * Check if the queue is empty
	 *
	 * @return true if queue is empty, false otherwise
	 */
	bool IsEmpty(void) {
		std::lock_guard<std::mutex> lock(m_mutex);

		return m_deque.empty();
	}

	/**
	 * Check if the queue is full
	 *
	 * @return true if queue is full, false otherwise
	 */
	bool IsFull(void) {
		std::lock_guard<std::mutex> lock(m_mutex);

		return m_deque.size() >= m_maxSize;
	}

	/**
	 * Get the current size of the queue
	 *
	 * @return Number of elements currently in the queue
	 */
	size_t Size(void) {
		std::lock_guard<std::mutex> lock(m_mutex);

		return m_deque.size();
	}

private:
	std::deque<T*> m_deque;    ///< Underlying deque container
	std::mutex m_mutex;        ///< Mutex for thread-safe access
	size_t m_maxSize;          ///< Maximum queue capacity
};

#endif
