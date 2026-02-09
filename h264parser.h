/**
 * @file h264parser.h
 * H264 parser header file
 *
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

#ifndef __H264PARSER_H
#define __H264PARSER_H

extern "C" {
#include <libavcodec/avcodec.h>
}

typedef enum {
	NALU_TYPE_NON_IDR = (1 << 0),
	NALU_TYPE_IDR     = (1 << 1),
	NALU_TYPE_SEI     = (1 << 2),
	NALU_TYPE_SPS     = (1 << 3),
	NALU_TYPE_PPS     = (1 << 4),
	NALU_TYPE_AUD     = (1 << 5)
} NalUnitTypes;

/**
 * cH264Parser - H264 Parser class
 */
class cH264Parser
{
public:
	cH264Parser(AVPacket *);
	int GetWidth(void) { return m_width; };
	int GetHeight(void) { return m_height; };
	bool IsIFrame(void);
	bool IsMbaff(void) { return m_mbaff; };

private:
	AVPacket *m_pAvpkt;
	const unsigned char *m_pStart;
	unsigned short m_nLength;
	int m_nCurrentBit;

	int m_nalutype = 0;

	int m_width = 0;
	int m_height = 0;
	bool m_mbaff = false;

	unsigned int ReadBit(void);
	unsigned int ReadBits(int);
	unsigned int ReadExponentialGolombCode(void);
	unsigned int ReadSE(void);
};

#endif
