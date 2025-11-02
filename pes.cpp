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
#include "logger.h"

#include "vdr/remux.h"

extern "C"
{
#include <libavutil/avutil.h>
}

/**
 * Construct a PES packet parser
 *
 * @param data    Pointer to the raw PES packet data
 * @param size    Size of the PES packet in bytes
 */
cPes::cPes(const uint8_t *data, int size)
	: m_data(data), m_size(size)
{
}

void cPes::Init() {
	if (IsHeaderValid() && (GetStreamId() & 0xF0) == GetExpectedStreamId()) {
		m_valid = true;
		int pesPayloadStart = PesPayloadOffset(m_data);

		if (pesPayloadStart + START_CODE_PREFIX_LEN + 1 >= (unsigned int)m_size) {
			LOGWARNING("pes: %s: packet too short", __FUNCTION__);

			return;
		}

		uint32_t firstThreePesPayloadBytes = ReadBytes(pesPayloadStart, START_CODE_PREFIX_LEN);
		const uint8_t *codecPayload = &m_data[pesPayloadStart + START_CODE_PREFIX_LEN];

		// Looking for the MPEG2 start code and stream type in the PES payload
		if (firstThreePesPayloadBytes == START_CODE_PREFIX && codecPayload[0] == MPEG2_STREAM_TYPE)
			m_codec = AV_CODEC_ID_MPEG2VIDEO;
		// Looking for a leading zero byte in front of the start code. Can be present in H.264/HEVC streams.
		else if (ReadBytes(pesPayloadStart + 1, START_CODE_PREFIX_LEN) == START_CODE_PREFIX) {
			codecPayload++;
			m_payloadHasLeadingZero = true;
		} else if (firstThreePesPayloadBytes != START_CODE_PREFIX) {
			return; // No start code: PES packet carries fragmented payload.
		}

		if (m_size > &codecPayload[7] - m_data) {
			if (     codecPayload[0] == H264_STREAM_TYPE && (codecPayload[1] == 0x10 || codecPayload[1] == 0xF0 || codecPayload[7] == 0x64))
				m_codec = AV_CODEC_ID_H264;
			else if (codecPayload[0] == HEVC_STREAM_TYPE && (codecPayload[1] == 0x10 || codecPayload[1] == 0x50 || codecPayload[7] == 0x40))
				m_codec = AV_CODEC_ID_HEVC;
		}
	} else {
		LOGERROR("pes: %s: invalid packet", __FUNCTION__);
	}
}

/**
 * Check if the PES packet is valid
 *
 * Validates that the PES packet is well-formed and matches the expected stream type by checking:
 * - The PES header is valid
 * - The stream ID matches the expected stream type (video or audio)
 *
 * The stream ID is masked with 0xF0 to check the stream type category (e.g., 0xE0 for video, 0xC0 for audio)
 * while ignoring the low nibble which indicates the specific stream number.
 *
 * Video streams have stream IDs in the range 0xE0-0xEF according to
 * H.222.0 03/2017 Table 2-22, audio streams have IDs in the range 0xC0-0xCF.
 *
 * @return true if the packet is valid and matches the expected stream type, false otherwise
 */
bool cPes::IsValid() {
	return m_valid;
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
 * Check if the PES packet contains a Presentation Time Stamp (PTS)
 *
 * Examines the PES header flags to determine if a PTS is present in the packet.
 * The PTS presence is indicated by specific bits in the PES header flags field.
 *
 * @return true if the PES packet contains a PTS, false otherwise
 */
bool cPes::HasPts()
{
	return PesHasPts(m_data);
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
	if (!HasPts())
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

/**
 * Get the total length of the PES packet
 *
 * Returns the complete size of the PES packet including both header and payload.
 * The length is read from the PES packet header (bytes 4-5).
 *
 * For packets with a specified length field (common for audio):
 *   - Returns the actual PES packet length from the header
 *   - Calculated as 6 + length_field (per H.222.0 standard)
 *   - The length_field specifies bytes after the 6-byte header prefix
 *     (3 bytes start code + 1 byte stream ID + 2 bytes length field)
 *
 * For unbounded packets (length field = 0, common for video streams):
 *   - Returns the input buffer size (m_size)
 *
 * @return Total size in bytes: actual packet length if specified, otherwise input buffer size
 */
int cPes::GetPacketLength()
{
	if (!PesHasLength(m_data))
		return m_size; // Length field is 0, meaning unbounded/unspecified. Return raw data size.

	return PesLength(m_data);
}
