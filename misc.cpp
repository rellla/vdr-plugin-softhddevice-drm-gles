/**
 * @file misc.cpp
 * Misc function implementation file
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

extern "C" {
#include <libavcodec/packet.h>
#include <libavcodec/defs.h>
}

#include "misc.h"
#include "logger.h"

AVPacket *CreateAvPacket(const uint8_t *data, int size, int64_t pts)
{
	AVPacket *avpkt = av_packet_alloc();

	if (!avpkt) {
		LOGFATAL("pes: %s: out of memory 1", __FUNCTION__);
		return nullptr;
	}

	if (av_new_packet(avpkt, size)) { // allocates size + AV_INPUT_BUFFER_PADDING_SIZE
		LOGFATAL("pes: %s: out of memory 2", __FUNCTION__);
		return nullptr;
	}

	memcpy(avpkt->data, data, size);
	memset(&avpkt->data[size], 0, AV_INPUT_BUFFER_PADDING_SIZE);
	avpkt->pts = pts;

	return avpkt;
}
