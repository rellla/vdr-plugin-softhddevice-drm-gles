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

#include <deque>
#include <mutex>
#include <cstdint>

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

	bool Push(const T& element);
	T Pop(void);
	T Head(void);
	T Peek(void);
	bool Empty(void);
	size_t Size(void);

private:
	std::deque<T> m_deque;    ///< Underlying deque container
	std::mutex m_mutex;       ///< Mutex for thread-safe access
	size_t m_maxSize;         ///< Maximum queue capacity
};

#endif
