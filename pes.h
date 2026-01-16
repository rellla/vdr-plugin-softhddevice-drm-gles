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

#include <vector>
#include <map>

#include <stdint.h>
#include <stddef.h>

extern "C"
{
#include <libavcodec/avcodec.h>
}

/**
 * PES packet parser class
 *
 * This class parses PES (Packetized Elementary Stream) packets
 * to extract header information, PTS, and payload data.
 */
class cPes
{
public:
	cPes(const uint8_t *, int);
	bool IsValid();
	bool HasPts();
	int64_t GetPts();
	const uint8_t *GetPayload();
	int GetPayloadSize();
	int GetPacketLength();
	uint8_t GetStreamId() { return m_data[3]; }

protected:
	virtual bool IsStreamIdValid() = 0;
	void Init();
	bool IsHeaderValid();

	bool m_valid = false;      ///< flag indicating if the PES packet is valid
	const uint8_t *m_data;     ///< pointer to the raw PES packet data
	int m_size;                ///< size of the PES packet

	// According to H.222.0 03/2017 Table 2-21 ("PES packet") packet_start_code_prefix
	// And also according to H.264/HEVC payload
	static constexpr uint32_t PES_PACKET_START_CODE_PREFIX = 0x00'0001;
	static constexpr uint32_t PES_PACKET_START_CODE_PREFIX_LEN = 3;
};

/**
 * Video PES packet parser
 *
 * Specialized parser for video PES packets with stream IDs in the range 0xE0-0xEF.
 */
class cPesVideo : public cPes {
public:
	cPesVideo(const uint8_t *data, int size) : cPes(data, size) { cPes::Init(); }
private:
	bool IsStreamIdValid() override { return (GetStreamId() & 0xF0) == 0xE0; } // Video stream IDs are in the range 0xE0-0xEF
};

/**
 * Audio PES packet parser
 *
 * Specialized parser for audio PES packets with stream IDs in the range 0xC0-0xCF,
 * or private stream ID 0xBD which may contain audio data.
 */
class cPesAudio : public cPes {
public:
	cPesAudio(const uint8_t *data, int size) : cPes(data, size) { cPes::Init(); }
	bool IsAudioStreamId() { return (GetStreamId() & 0xF0) == 0xC0; } // Audio stream IDs are in the range 0xC0-0xCF
private:
	bool IsStreamIdValid() override { return IsAudioStreamId() || IsPrivateStreamId(); }
	bool IsPrivateStreamId() { return GetStreamId() == 0xBD; }
};

/**
 * Buffer that tracks PTS values at specific byte positions
 *
 * Manages a byte buffer along with a map of PTS (Presentation Time Stamp) values
 * associated with specific positions in the buffer. This is used for maintaining
 * temporal information when reassembling fragmented streams.
 */
class cPtsTrackingBuffer {
public:
	cPtsTrackingBuffer(const char *identifier) : m_identifier(identifier) {}
	void Push(const uint8_t *, int, int64_t);
	void Erase(size_t);
	int64_t GetPts();
	const uint8_t *Peek() { return &m_data[0]; }
	void Reset() { m_data.clear(); m_pts.clear(); }
	int GetSize() { return m_data.size(); }
	const char *GetIdentifier() { return m_identifier; }
private:
	const char *m_identifier;
	std::map<size_t, int64_t> m_pts;     ///< Map of buffer positions to PTS values
	std::vector<uint8_t> m_data;         ///< Byte buffer
};

/**
 * Base class for stream reassembly buffers
 *
 * Reassembles fragmented elementary streams into complete AVPackets.
 * Handles codec detection and PTS tracking across fragments.
 */
class cReassemblyBuffer {
public:
	virtual void Push(const uint8_t *data, int size, int64_t pts) { m_buffer.Push(data, size, pts); }
	virtual AVPacket *PopAvPacket() = 0;
	bool IsEmpty() { return m_buffer.GetSize() == 0; }
	size_t GetSize() { return m_buffer.GetSize(); }
	void Reset();
	AVCodecID GetCodec() { return m_codec; }
protected:
	cReassemblyBuffer(const char *identifier) : m_buffer(identifier) {}
	AVPacket *PopAvPacket(int);
	AVCodecID m_codec = AV_CODEC_ID_NONE;          ///< detected codec ID
	cPtsTrackingBuffer m_buffer;                   ///< fragmentation buffer
	int64_t m_lastPoppedPts = AV_NOPTS_VALUE;      ///< PTS of the last popped AVPacket
};

/**
 * Video stream reassembly buffer
 *
 * Reassembles video elementary streams (MPEG2, H.264, HEVC) by detecting
 * frame start codes and codec headers.
 */
class cReassemblyBufferVideo : public cReassemblyBuffer {
public:
	cReassemblyBufferVideo() : cReassemblyBuffer("vid") {}
	AVPacket *PopAvPacket() override { return cReassemblyBuffer::PopAvPacket(m_buffer.GetSize()); }
	bool ParseCodecHeader(const uint8_t *, int);
	bool HasLeadingZero(const uint8_t *, int);
private:
	static constexpr uint32_t VIDEO_FRAME_START_CODE = 0x00'0001;
	static constexpr int VIDEO_FRAME_START_CODE_LEN = 3;

	static constexpr uint8_t MPEG2_STREAM_TYPE = 0xB3;
	static constexpr uint8_t H264_STREAM_TYPE = 0x09;
	static constexpr uint8_t HEVC_STREAM_TYPE = 0x46;
};

/**
 * Information about a detected audio sync word
 */
struct SyncWordInfo {
	AVCodecID codecId;     ///< Detected codec ID
	int pos;               ///< Position of sync word in buffer
};

/**
 * Audio stream reassembly buffer
 *
 * Reassembles audio elementary streams by detecting sync words and validating
 * frame headers. Supports MP2, AAC (LATM/ADTS), AC3, and E-AC3 codecs.
 */
class cReassemblyBufferAudio : public cReassemblyBuffer {
public:
	cReassemblyBufferAudio() : cReassemblyBuffer("AUDIO") {}
	AVPacket *PopAvPacket() override;
	AVCodecID TruncateBufferUntilFirstValidData();
	SyncWordInfo FindSyncWord(const uint8_t *, int );
	AVCodecID DetectCodecFromSyncWord(const uint8_t *, int);
	int GetFrameSizeForCodec(AVCodecID, const uint8_t *);
private:
	SyncWordInfo FindTwoConsecutiveFramesWithSameSyncWord();
	static constexpr int MAX_HEADER_SIZE = 6;
	bool m_ptsInvalid = false;   ///< flag indicating if PTS is invalid for current buffer, because it was truncated
};

#endif
