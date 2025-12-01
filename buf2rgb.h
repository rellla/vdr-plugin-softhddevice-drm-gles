/**
 * @file buf2rgb.h
 * Some helper functions header file
 *
 * @copyright: (c) 2025 by Andreas Baierl. All Rights Reserved.
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

#ifndef __BUF2RGB_H
#define __BUF2RGB_H

#include "drmbuffer.h"

/****************************************************************************************
 * Helpers
 ***************************************************************************************/
uint8_t *BufToRgb(cDrmBuffer *buf, int *size, int w, int h, enum AVPixelFormat dst_pix_fmt);
uint8_t *ScaleRgb24(uint8_t *src, int *size, int src_w, int src_h, int dst_w, int dst_h);
void AlphaBlend(uint8_t *result, uint8_t *front, uint8_t *back, const unsigned int width, const unsigned int height);
int BlitVideo(uint8_t *dst, uint8_t *src, int dst_w, int dst_h, int dst_x, int dst_y, int src_w, int src_h);
void PrintStreamData(const uint8_t *data, int size);

#endif
