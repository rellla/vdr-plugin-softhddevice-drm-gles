/**
 * @file pool.h
 * Pool class header file
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

#ifndef __SOFTHDPOOL_H
#define __SOFTHDPOOL_H

#include <vector>
#include <memory>

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

#endif
