/**
 * @file pes.h
 * PES packet parser header
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

#ifndef __SOFTHDDEVICE_PES_H
#define __SOFTHDDEVICE_PES_H

#include <stdint.h>
#include <stddef.h>

extern "C"
{
#include <libavcodec/avcodec.h>
}

/**
 * Convert AVCodecID to a human-readable string
 *
 * @param c    The codec ID to convert
 * @return A   string representation of the codec ID
 */
constexpr const char* to_string(AVCodecID c) {
    switch (c) {
        case AV_CODEC_ID_NONE: return "None";
        case AV_CODEC_ID_MPEG2VIDEO: return "MPEG2";
        case AV_CODEC_ID_H264: return "H.264";
        case AV_CODEC_ID_HEVC: return "HEVC";
		default: return "Unknown codec";
    }
}

/**
 * PES packet parser class
 *
 * This class parses PES (Packetized Elementary Stream) packets
 * to extract header information, video codec, PTS, and payload data.
 */
class cPes
{
public:
	cPes(const uint8_t *data, int size);
	bool IsValid();
	AVCodecID GetCodec() { return m_codec; }
	bool HasPts();
	int64_t GetPts();
	const uint8_t *GetPayload();
	int GetPayloadSize();
	int GetPacketLength();

protected:
	void Init();
	bool IsHeaderValid();
	uint32_t ReadBytes(int offset, int count);
	uint8_t GetStreamId() { return m_data[3]; }
	virtual uint8_t GetExpectedStreamId() = 0;

	// According to H.222.0 03/2017 Table 2-21 packet_start_code_prefix
	// And also according to H.264/HEVC payload
	static constexpr uint32_t START_CODE_PREFIX = 0x00'0001;
	static constexpr uint32_t START_CODE_PREFIX_LEN = 3;

	static constexpr uint8_t MPEG2_STREAM_TYPE = 0xB3;
	static constexpr uint8_t H264_STREAM_TYPE = 0x09;
	static constexpr uint8_t HEVC_STREAM_TYPE = 0x46;

	bool m_valid = false;                  ///< flag indicating if the PES packet is valid
	const uint8_t *m_data;                 ///< pointer to the raw PES packet data
	int m_size;                            ///< size of the PES packet
	bool m_payloadHasLeadingZero = false;  ///< flag indicating if codec payload has leading zero byte for H.264/HEVC
	AVCodecID m_codec = AV_CODEC_ID_NONE;  ///< detected codec ID
};

class cPesVideo : public cPes {
public:
	cPesVideo(const uint8_t *data, int size) : cPes(data, size) { cPes::Init(); }
private:
	uint8_t GetExpectedStreamId() override { return 0xE0; } // Video stream IDs are in the range 0xE0-0xEF
};

class cPesAudio : public cPes {
public:
	cPesAudio(const uint8_t *data, int size) : cPes(data, size) { cPes::Init(); }
private:
	uint8_t GetExpectedStreamId() override { return 0xC0; } // Audio stream IDs are in the range 0xC0-0xCF
};

#endif
