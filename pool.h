// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file pool.h
 * Pool Implementation
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __SOFTHDPOOL_H
#define __SOFTHDPOOL_H

#include <memory>
#include <vector>

/**
 * @addtogroup misc
 * {
 */

/**
 * Pool Implementation Template Class
 */
template <typename T>
class cPool {
protected:
	std::vector<std::unique_ptr<T>> buffer;
	size_t currentIndex = 0;

public:
	cPool(size_t size) {
		buffer.reserve(size);

		for (size_t i = 0; i < size; ++i) {
				buffer.emplace_back(std::make_unique<T>());
		}
	}
};

/** @} */

#endif
