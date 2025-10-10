/**
 * @file pes.cpp
 * PES packet parser implementation
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

#include "pes.h"

#include "vdr/remux.h"

extern "C"
{
#include <libavutil/avutil.h>
}

/**
 * Construct a PES packet parser
 *
 * @param data Pointer to the raw PES packet data
 * @param size Size of the PES packet in bytes
 */
cPes::cPes(const uint8_t *data, int size)
	: m_data(data), m_size(size)
{
}

/**
 * Parse the PES packet to detect the video codec
 *
 * Analyzes the PES payload to identify the codec type (MPEG2, H.264, or HEVC).
 * This method looks for codec-specific start codes and stream type identifiers
 * in the payload. The detected codec can be retrieved using GetCodec().
 *
 * Supported codecs:
 * - MPEG2 (AV_CODEC_ID_MPEG2VIDEO)
 * - H.264 (AV_CODEC_ID_H264)
 * - HEVC (AV_CODEC_ID_HEVC)
 *
 * @note This method must be called before GetCodec()
 */
void cPes::Parse() {
	int pesPayloadStart = PesPayloadOffset(m_data);

	if (pesPayloadStart + START_CODE_PREFIX_LEN + 1 >= (unsigned int)m_size)
		return;

	uint32_t firstThreePesPayloadBytes = ReadBytes(pesPayloadStart, START_CODE_PREFIX_LEN);
	const uint8_t *codecPayload = &m_data[pesPayloadStart + START_CODE_PREFIX_LEN];

	// Looking for the MPEG2 start code and stream type
	if (firstThreePesPayloadBytes == START_CODE_PREFIX && codecPayload[0] == MPEG2_STREAM_TYPE)
		m_codec = AV_CODEC_ID_MPEG2VIDEO;
	// Looking for the H.264/HEVC start code. It can have an optional leading zero byte.
	else if (ReadBytes(pesPayloadStart + 1, START_CODE_PREFIX_LEN) == START_CODE_PREFIX) {
		codecPayload++;
		m_payloadHasLeadingZero = true;
	} else if (firstThreePesPayloadBytes != START_CODE_PREFIX)
		return; // no start code found

	if (m_size > &codecPayload[7] - m_data) {
		if (     codecPayload[0] == H264_STREAM_TYPE && (codecPayload[1] == 0x10 || codecPayload[1] == 0xF0 || codecPayload[7] == 0x64))
			m_codec = AV_CODEC_ID_H264;
		else if (codecPayload[0] == HEVC_STREAM_TYPE && (codecPayload[1] == 0x10 || codecPayload[1] == 0x50 || codecPayload[7] == 0x40))
			m_codec = AV_CODEC_ID_HEVC;
	}
}

uint32_t cPes::ReadBytes(int offset, int count)
{
	uint32_t value = 0;

	for (int i = 0; i < count; i++) {
		value <<= 8;
		value |= m_data[offset + i];
	}

	return value;
}

/**
 * Check if the PES header is valid
 *
 * Validates that the PES packet has a valid header by checking:
 * - The packet is long enough to contain a header
 * - The start code prefix (0x000001) is present
 *
 * @return true if the header is valid, false otherwise
 */
bool cPes::IsHeaderValid()
{
	return PesLongEnough(m_size) && ReadBytes(0, 3) == START_CODE_PREFIX;
}

/**
 * Check if this is a video stream
 *
 * Determines if the PES packet contains a video stream based on the stream ID.
 * Video streams have stream IDs in the range 0xE0-0xEF according to
 * H.222.0 03/2017 Table 2-22.
 *
 * @return true if this is a video stream, false otherwise
 */
bool cPes::IsVideoStream()
{
	// The low nibble is the stream number
	return (GetStreamId() & 0xF0) == 0xE0;
}

/**
 * Check if this is an audio stream
 *
 * Determines if the PES packet contains an audio stream based on the stream ID.
 * Audio streams have stream IDs in the range 0xC0-0xCF according to
 * H.222.0 03/2017 Table 2-22.
 *
 * @return true if this is an audio stream, false otherwise
 */
bool cPes::IsAudioStream()
{
	// The low nibble is the stream number
	return (GetStreamId() & 0xF0) == 0xC0;
}

/**
 * Get the Presentation Time Stamp (PTS) from the PES header
 *
 * Extracts the PTS value from the PES packet header if present.
 * The PTS indicates when the decoded content should be presented.
 *
 * @return The PTS value in 90 kHz units, or AV_NOPTS_VALUE if no PTS is present
 */
int64_t cPes::GetPts()
{
	if (!PesHasPts(m_data))
		return AV_NOPTS_VALUE;

	return PesGetPts(m_data);
}

/**
 * Get a pointer to the PES payload data
 *
 * Returns a pointer to the start of the payload data, skipping the PES header.
 * For H.264/HEVC streams with a leading zero byte, the leading zero is also skipped.
 *
 * @return Pointer to the payload data
 */
const uint8_t *cPes::GetPayload()
{
	int headerLen = PesPayloadOffset(m_data);

	if (m_payloadHasLeadingZero)
		headerLen++; // Skip the leading zero byte for H.264/HEVC

	return &m_data[headerLen];
}

/**
 * Get the size of the PES payload
 *
 * Calculates the size of the payload by subtracting the header size
 * (and optional leading zero for H.264/HEVC) from the total packet size.
 *
 * @return Size of the payload in bytes
 */
int cPes::GetPayloadSize()
{
	return m_size - (GetPayload() - m_data);
}
