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

#include <functional>
#include <map>
#include <stdexcept>

#include "pes.h"
#include "logger.h"

#include "vdr/remux.h"

extern "C" {
#include <libavutil/avutil.h>
}

static uint32_t ReadBytes(const uint8_t *data, int count)
{
	uint32_t value = 0;

	for (int i = 0; i < count; i++) {
		value <<= 8;
		value |= data[i];
	}

	return value;
}

/**
 * Codec information structure
 * Contains lambdas for sync word detection and frame size calculation
 */
struct CodecInfo {
	int minSize;
	std::function<bool(const uint8_t*)> MatchSyncWord;
	std::function<int(const uint8_t*)> GetFrameSize;
};

/**
 * Map of audio codec information
 * Key: AVCodecID
 * Value: CodecInfo with sync word detection and frame size calculation lambdas
 */
static const std::map<AVCodecID, CodecInfo> AudioCodecMap = {
	{AV_CODEC_ID_MP2, {
		.minSize = 3,
		.MatchSyncWord = [](const uint8_t* data) -> bool {
			constexpr uint32_t MPEG_AUDIO_SYNC_WORD = 0xFF'E000;
			constexpr uint32_t MPEG_AUDIO_VERSION_FORBIDDEN_VALUE = 0x00'0800;
			constexpr uint32_t MPEG_AUDIO_LAYER_DESCRIPTION_FORBIDDEN_VALUE = 0x00'0000;
			constexpr uint32_t MPEG_AUDIO_BITRATE_INDEX_FORBIDDEN_VALUE = 0x00'00F0;

			uint32_t syncWord = ReadBytes(data, 3);
			return (syncWord & 0b1111'1111'1110'0000'0000'0000) == MPEG_AUDIO_SYNC_WORD &&
			       (syncWord & 0b0000'0000'0001'1000'0000'0000) != MPEG_AUDIO_VERSION_FORBIDDEN_VALUE &&
			       (syncWord & 0b0000'0000'0000'0110'0000'0000) != MPEG_AUDIO_LAYER_DESCRIPTION_FORBIDDEN_VALUE &&
			       (syncWord & 0b0000'0000'0000'0000'1111'0000) != MPEG_AUDIO_BITRATE_INDEX_FORBIDDEN_VALUE;
		},
		.GetFrameSize = [](const uint8_t* data) -> int {
			constexpr uint16_t BitRateTable[2][4][16] = {
				// MPEG Version 1
				{{},
				{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
				{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
				{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
				// MPEG Version 2 & 2.5
				{{},
				{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
				{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
				{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}
				}
			};
			constexpr uint16_t SampleRateTable[4] = {44100, 48000, 32000, 0};

			int mpeg2 = !(data[1] & 0x08) && (data[1] & 0x10);
			int mpeg25 = !(data[1] & 0x08) && !(data[1] & 0x10);
			int layer = 4 - ((data[1] >> 1) & 0x03);
			int bitRateIndex = (data[2] >> 4) & 0x0F;
			int sampleRateIndex = (data[2] >> 2) & 0x03;
			int padding = (data[2] >> 1) & 0x01;

			int sampleRate = SampleRateTable[sampleRateIndex];
			if (!sampleRate)
				throw std::invalid_argument("MPEG: invalid sample rate");

			sampleRate >>= mpeg2;
			sampleRate >>= mpeg25;

			int bitRate = BitRateTable[mpeg2 | mpeg25][layer][bitRateIndex];
			if (!bitRate)
				throw std::invalid_argument("MPEG: invalid bit rate");

			bitRate *= 1000;
			int frameSize;
			switch (layer) {
				case 1:
					frameSize = (12 * bitRate) / sampleRate;
					frameSize = (frameSize + padding) * 4;
					break;
				case 2:
				case 3:
				default:
					frameSize = (144 * bitRate) / sampleRate;
					frameSize = frameSize + padding;
					break;
			}
			return frameSize;
		}
	}},
	{AV_CODEC_ID_AAC_LATM, {
		.minSize = 3,
		.MatchSyncWord = [](const uint8_t* data) -> bool {
			constexpr uint32_t LAOS_SYNC_WORD_MASK = 0xFFE000;
			constexpr uint32_t LAOS_SYNC_WORD = 0x2B7 << (24-11);

			uint32_t syncWord = ReadBytes(data, 3);
			return (syncWord & LAOS_SYNC_WORD_MASK) == LAOS_SYNC_WORD;
		},
		.GetFrameSize = [](const uint8_t* data) -> int {
			return ((data[1] & 0x1F) << 8) + data[2] + 3;
		}
	}},
	{AV_CODEC_ID_AC3, {
		.minSize = 6,
		.MatchSyncWord = [](const uint8_t* data) -> bool {
			constexpr uint32_t AC3_SYNC_WORD_MASK = 0xFFFF00;
			constexpr uint32_t AC3_SYNC_WORD = 0x0B77 << (24-16);

			uint32_t syncWord = ReadBytes(data, 3);
			return (syncWord & AC3_SYNC_WORD_MASK) == AC3_SYNC_WORD &&
			       data[5] <= (10 << 3);
		},
		.GetFrameSize = [](const uint8_t* data) -> int {
			constexpr uint16_t Ac3FrameSizeTable[38][3] = {
				{64, 69, 96}, {64, 70, 96}, {80, 87, 120}, {80, 88, 120},
				{96, 104, 144}, {96, 105, 144}, {112, 121, 168}, {112, 122, 168},
				{128, 139, 192}, {128, 140, 192}, {160, 174, 240}, {160, 175, 240},
				{192, 208, 288}, {192, 209, 288}, {224, 243, 336}, {224, 244, 336},
				{256, 278, 384}, {256, 279, 384}, {320, 348, 480}, {320, 349, 480},
				{384, 417, 576}, {384, 418, 576}, {448, 487, 672}, {448, 488, 672},
				{512, 557, 768}, {512, 558, 768}, {640, 696, 960}, {640, 697, 960},
				{768, 835, 1152}, {768, 836, 1152}, {896, 975, 1344}, {896, 976, 1344},
				{1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728},
				{1152, 1254, 1728}, {1280, 1393, 1920}, {1280, 1394, 1920},
			};

			int fscod = data[4] >> 6;
			if (fscod == 0x03)
				throw std::invalid_argument("AC3: invalid sample rate");

			int frmsizcod = data[4] & 0x3F;
			if (frmsizcod > 37)
				throw std::invalid_argument("AC3: invalid frame size");

			return Ac3FrameSizeTable[frmsizcod][fscod] * 2;
		}
	}},
	{AV_CODEC_ID_EAC3, {
		.minSize = 6,
		.MatchSyncWord = [](const uint8_t* data) -> bool {
			constexpr uint32_t AC3_SYNC_WORD = 0x0B77 << (24-16);

			uint32_t syncWord = ReadBytes(data, 3);
			return (syncWord & 0xFFFF00) == AC3_SYNC_WORD && data[5] > (10 << 3);
		},
		.GetFrameSize = [](const uint8_t* data) -> int {
			if ((data[4] & 0xF0) == 0xF0)
				throw std::invalid_argument("AC3: invalid fscod fscod2");

			return (((data[2] & 0x07) << 8) + data[3] + 1) * 2;
		}
	}},
	{AV_CODEC_ID_AAC, {
		.minSize = 3,
		.MatchSyncWord = [](const uint8_t* data) -> bool {
			constexpr uint32_t ADTS_SYNC_WORD = 0xFFF000;
			constexpr uint32_t ADTS_LAYER = 0x000000;
			constexpr uint32_t ADTS_SAMPLING_FREQUENCY_FORBIDDEN_VALUE = 15 << 6;

			uint32_t syncWord = ReadBytes(data, 3);
			return (syncWord & 0b1111'1111'1111'0000'0000'0000) == ADTS_SYNC_WORD &&
			       (syncWord & 0b0000'0000'0000'0110'0000'0000) == ADTS_LAYER &&
			       (syncWord & 0b0000'0000'0000'0011'1100'0000) != ADTS_SAMPLING_FREQUENCY_FORBIDDEN_VALUE;
		},
		.GetFrameSize = [](const uint8_t* data) -> int {
			return ((data[3] & 0x03) << 11) | ((data[4] & 0xFF) << 3) | ((data[5] & 0xE0) >> 5);
		}
	}}
};

/**
 * Construct a PES packet parser
 *
 * Initializes the parser with a pointer to PES packet data and its size.
 * The actual validation is performed by calling Init() in derived classes.
 *
 * @param data     Pointer to the raw PES packet data
 * @param size     Size of the PES packet in bytes
 */
cPes::cPes(const uint8_t *data, int size)
	: m_data(data), m_size(size)
{
}

/**
 * Initialize and validate the PES packet
 *
 * Performs validation checks on the PES packet structure:
 * - Validates the PES header (start code prefix)
 * - Checks if the stream ID matches the expected type (audio/video)
 * - Ensures the packet has sufficient size for the header
 * - Verifies the payload offset is within bounds
 *
 * Sets m_valid to true if all checks pass. Called by derived class constructors.
 */
void cPes::Init() {
	if (IsHeaderValid() && IsStreamIdValid()) {
		if (m_size <= 8 || PesPayloadOffset(m_data) > m_size) // header length field is at position 8 when the PES extension is present
			LOGWARNING("pes: %s: packet too short: %d %02X", __FUNCTION__, m_size, GetStreamId());
		else
			m_valid = true;
	} else
		LOGDEBUG("pes: %s: invalid packet: %d %02X", __FUNCTION__, m_size, GetStreamId());
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
	return PesLongEnough(m_size) && ReadBytes(m_data, 3) == PES_PACKET_START_CODE_PREFIX;
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
	return &m_data[PesPayloadOffset(m_data)];
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
	return m_size - PesPayloadOffset(m_data);
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

#include "misc.h"
/**
 * Pop an AVPacket from the reassembly buffer
 *
 * Creates an AVPacket containing the specified amount of data from the buffer.
 *
 * @param size     Number of bytes to pop from the buffer
 *
 * @return Allocated AVPacket with data, or nullptr if size is 0
 */
AVPacket *cReassemblyBuffer::PopAvPacket(int size)
{
	if (size == 0)
		return nullptr;

	AVPacket *avpkt = av_packet_alloc();

	if (!avpkt)
		LOGFATAL("pes: %s: out of memory while allocating AVPacket", __FUNCTION__);

	if (av_new_packet(avpkt, size)) // allocates size + AV_INPUT_BUFFER_PADDING_SIZE
		LOGFATAL("pes: %s: out of memory while allocating AVPacket payload", __FUNCTION__);

	memcpy(avpkt->data, m_buffer.Peek(), size);
	memset(&avpkt->data[size], 0, AV_INPUT_BUFFER_PADDING_SIZE);

	// Only audio:
	// If a PES packet contains multiple frames, only the AVPacket with the first frame of that PES packet shall have a PTS value, when sending it to the decoder.
	// The following AVPackets created from this PES packet shall have no PTS value.
	// When retrieving PTS values from the same PES packet, they will be identical.
	if (m_buffer.GetPts() != m_lastPoppedPts)
		avpkt->pts = m_buffer.GetPts();
	else
		avpkt->pts = AV_NOPTS_VALUE;

	m_lastPoppedPts = m_buffer.GetPts();

	m_buffer.Erase(size);

	return avpkt;
}

/*
 * Video specific implementation
 */

/**
 * Parse video codec header to detect codec type
 *
 * Analyzes video frame start codes and stream type bytes to identify the codec.
 * Supports MPEG2, H.264, and HEVC video codecs.
 *
 * @param fragment     Pointer to video frame data
 * @param size         Size of the fragment in bytes
 *
 * @return true if a codec was detected, false otherwise
 */
bool cReassemblyBufferVideo::ParseCodecHeader(const uint8_t *fragment, int size) {
	const uint8_t *codecPayload = &fragment[VIDEO_FRAME_START_CODE_LEN];
	uint32_t startCode = ReadBytes(fragment, VIDEO_FRAME_START_CODE_LEN);

	// Looking for the MPEG2 start code and stream type in the PES payload
	if (startCode == VIDEO_FRAME_START_CODE && codecPayload[0] == MPEG2_STREAM_TYPE)
		m_codec = AV_CODEC_ID_MPEG2VIDEO;
	else if (HasLeadingZero(fragment, size)) // Looking for a leading zero byte in front of the start code. Can be present in H.264/HEVC streams.
		codecPayload++;
	else if (startCode != VIDEO_FRAME_START_CODE)
		return false; // No start code: PES packet carries fragmented payload, or unknown codec.

	if (size > &codecPayload[7] - fragment) {
		if (     codecPayload[0] == H264_STREAM_TYPE && (codecPayload[1] == 0x10 || codecPayload[1] == 0xF0 || codecPayload[7] == 0x64))
			m_codec = AV_CODEC_ID_H264;
		else if (codecPayload[0] == HEVC_STREAM_TYPE && (codecPayload[1] == 0x10 || codecPayload[1] == 0x50 || codecPayload[7] == 0x40))
			m_codec = AV_CODEC_ID_HEVC;
	}

	return m_codec != AV_CODEC_ID_NONE;
}

/**
 * Check if video data has a leading zero byte before the start code
 *
 * Some H.264/HEVC streams include a leading zero byte (0x00) before
 * the standard start code (0x000001). This method detects that pattern.
 *
 * @param data     Pointer to video data
 * @param size     Size of the data in bytes
 *
 * @return true if a leading zero is present, false otherwise
 */
bool cReassemblyBufferVideo::HasLeadingZero(const uint8_t *data, int size)
{
	return size > VIDEO_FRAME_START_CODE_LEN + 1 && data[0] == 0 && ReadBytes(&data[1], VIDEO_FRAME_START_CODE_LEN) == VIDEO_FRAME_START_CODE;
}

/*
 * Audio specific implementation
 */

/**
 * Pop an audio AVPacket from the reassembly buffer
 *
 * Truncates the buffer to the first valid audio frame, detects the codec,
 * and pops a complete audio frame as an AVPacket.
 *
 * @return Allocated AVPacket with one audio frame, or nullptr if no valid frame found
 */
AVPacket *cReassemblyBufferAudio::PopAvPacket()
{
	AVCodecID detectedCodec = TruncateBufferUntilFirstValidData();

	if (detectedCodec == AV_CODEC_ID_NONE)
		return nullptr; // No sync word found in the buffer. Wait for more data.
	else if (m_codec != AV_CODEC_ID_NONE && detectedCodec != m_codec)
		LOGERROR("pes: %s: audio codec changed unexpectedly from %d to %d", __FUNCTION__, avcodec_get_name(m_codec), avcodec_get_name(detectedCodec));

	m_codec = detectedCodec;

	try {
		AVPacket *packet = cReassemblyBuffer::PopAvPacket(AudioCodecMap.at(m_codec).GetFrameSize(m_buffer.Peek()));

		if (m_ptsInvalid) { // the PTS is invalid for this packet because the buffer was truncated before
			packet->pts = AV_NOPTS_VALUE;

			m_ptsInvalid = false;
		}

		return packet;
	} catch (const std::invalid_argument &e) {
		LOGWARNING("pes: %s: garbage in audio stream received: %s", __FUNCTION__, e.what());
		// the garbage will be removed in the next call to TruncateBufferUntilFirstValidData()
	}

	return nullptr;
}

/**
 * Truncate buffer until the first valid audio frame
 *
 * Searches for two consecutive audio frames with the same sync word to validate
 * the frame header, then erases all data before the first valid frame.
 * This removes garbage data and synchronizes to the audio stream.
 *
 * @return Detected codec ID, or AV_CODEC_ID_NONE if no valid frames found
 */
AVCodecID cReassemblyBufferAudio::TruncateBufferUntilFirstValidData() {
	int sizeBeforeTruncation = m_buffer.GetSize();

	SyncWordInfo firstFrame = FindTwoConsecutiveFramesWithSameSyncWord();

	m_buffer.Erase(firstFrame.pos);

	if (m_buffer.GetSize() < sizeBeforeTruncation) {
		LOGDEBUG("pes: %s: truncated %d of %d bytes while searching for sync word", __FUNCTION__, sizeBeforeTruncation - m_buffer.GetSize(), sizeBeforeTruncation);
		m_ptsInvalid = true;
	}

	return firstFrame.codecId;
}

/**
 * Find two consecutive audio frames with the same sync word
 *
 * Searches the buffer for a valid audio frame followed immediately by another
 * frame of the same codec. This validates that the sync word and frame header
 * are genuine and not false positives in random data.
 *
 * The function modifies the buffer internally by erasing false positives as it searches.
 * When no sync word is found, it keeps the last MAX_HEADER_SIZE bytes.
 *
 * @return SyncWordInfo with codec ID and position, or AV_CODEC_ID_NONE if not found
 */
SyncWordInfo cReassemblyBufferAudio::FindTwoConsecutiveFramesWithSameSyncWord()
{
	while (true) {
		SyncWordInfo firstFrame = FindSyncWord(m_buffer.Peek(), m_buffer.GetSize());

		if (firstFrame.codecId == AV_CODEC_ID_NONE) // No sync word found in the entire buffer. Keep only the last few bytes that could contain a partial sync word.
			return SyncWordInfo{AV_CODEC_ID_NONE, std::max(0, (int)m_buffer.GetSize() - MAX_HEADER_SIZE)};

		try {
			// determine the length of the first found potential frame by reading the frame's header
			int sizeOfFirstFrame = AudioCodecMap.at(firstFrame.codecId).GetFrameSize(&m_buffer.Peek()[firstFrame.pos]);
			int secondSyncWord = firstFrame.pos + sizeOfFirstFrame;

			// check if another sync word follows immediately after the first frame to validate the header of the first frame is a real header and no random data
			if (secondSyncWord + MAX_HEADER_SIZE > (int)m_buffer.GetSize()) {
				// Could not find the second sync word, because there might not be enough data in the buffer to contain a complete second sync word. Wait for more data.
				// In case we have a false positive and the header's frame size field is invalid, we buffer the following amount of data in worst-case:
				//   - MP2: 6913 bytes (Layer 2/3: 384kbps @ 8kHz + padding)
				//   - AAC LATM: 8194 bytes (13-bit length field max: 0x1FFF + 3)
				//   - AC3: 2788 bytes (frmsizcod=37, fscod=1: 1394 * 2)
				//   - EAC3: 4096 bytes (11-bit field max: 2048 * 2)
				//   - AAC ADTS: 8191 bytes (13-bit length field max: 0x1FFF)
				return SyncWordInfo{AV_CODEC_ID_NONE, firstFrame.pos};
			} else if (DetectCodecFromSyncWord(&m_buffer.Peek()[secondSyncWord], m_buffer.GetSize() - secondSyncWord) == firstFrame.codecId)
				// two consecutive frames with the same sync word found, and the first frame's header length field is valid
				return SyncWordInfo{firstFrame.codecId, firstFrame.pos};
		} catch (const std::invalid_argument &e) {
			// Failed to read the frame size from the first frame's header. The found sync word is a false positive.
		}

		// If we found one sync word, but did not find a second one at the expected position, the first one was a false positive in the middle of random data.
		// In this case, continue the search one position after the start of the first found sync word.
		m_buffer.Erase(firstFrame.pos + 1);
	}
}

/**
 * Find the first audio sync word in data
 *
 * Scans the data byte-by-byte looking for any recognized audio sync word pattern.
 * Checks all supported audio codecs (MP2, AAC LATM, AAC ADTS, AC3, E-AC3).
 *
 * @param data Pointer to audio data
 * @param size Size of the data in bytes
 *
 * @return SyncWordInfo with detected codec and position, or AV_CODEC_ID_NONE if not found
 */
SyncWordInfo cReassemblyBufferAudio::FindSyncWord(const uint8_t *data, int size)
{
	for (int i = 0; i < size; i++) {
		AVCodecID detectedCodec = DetectCodecFromSyncWord(&data[i], size - i);
		if (detectedCodec != AV_CODEC_ID_NONE)
			return SyncWordInfo{detectedCodec, i};
	}

	return SyncWordInfo{AV_CODEC_ID_NONE, -1};
}

/**
 * Detect audio codec from sync word pattern
 *
 * Checks if the data starts with a valid sync word for any supported audio codec.
 * Uses the AudioCodecMap to test sync word patterns for MP2, AAC LATM, AAC ADTS, AC3, and E-AC3.
 *
 * @param syncWord Pointer to potential sync word data
 * @param size Size of available data
 *
 * @return Detected AVCodecID, or AV_CODEC_ID_NONE if no match
 */
AVCodecID cReassemblyBufferAudio::DetectCodecFromSyncWord(const uint8_t *syncWord, int size)
{
	for (const auto& [codecId, codecInfo] : AudioCodecMap) {
		if (size >= codecInfo.minSize && codecInfo.MatchSyncWord(syncWord)) {
			return codecId;
		}
	}

	return AV_CODEC_ID_NONE;
}

/**
 * Get the frame size for a given codec and frame header
 *
 * Calculates the frame size by parsing the codec-specific frame header.
 * Only used for testing purposes to expose the AudioCodecMap frame size calculation.
 *
 * @param codec The audio codec ID
 * @param data Pointer to the frame header data
 *
 * @return Frame size in bytes
 * @throws std::out_of_range if codec is not in AudioCodecMap
 * @throws std::invalid_argument if frame header is invalid
 */
int cReassemblyBufferAudio::GetFrameSizeForCodec(AVCodecID codec, const uint8_t *data)
{
	return AudioCodecMap.at(codec).GetFrameSize(data);
}

/**
 * Reset the reassembly buffer
 *
 * Clears all buffered data, PTS tracking, and resets codec detection state.
 */
void cReassemblyBuffer::Reset()
{
	m_buffer.Reset();
	m_codec = AV_CODEC_ID_NONE;
	m_lastPoppedPts = AV_NOPTS_VALUE;
}

/*
 * PTS tracking buffer
 */

/**
 * Push data into the PTS tracking buffer
 *
 * Appends data to the buffer and associates the PTS with the current buffer position
 * if a valid PTS is provided.
 *
 * @param data Pointer to data to append
 * @param size Size of data in bytes
 * @param pts Presentation timestamp, or AV_NOPTS_VALUE if not available
 */
void cPtsTrackingBuffer::Push(const uint8_t *data, int size, int64_t pts) {
	if (pts != AV_NOPTS_VALUE) // PES packets not starting with a new frame (fragmented data) have no PTS
		m_pts[m_data.size()] = pts;

	m_data.insert(m_data.end(), data, data + size);
}

/**
 * Erase data from the beginning of the buffer
 *
 * Removes the specified number of bytes from the front of the buffer and adjusts
 * all PTS positions accordingly. The PTS value for the new position 0 is preserved
 * by finding the last PTS value before the erase point.
 *
 * This ensures that when frames are popped from the buffer, they retain the PTS
 * of the PES packet where the frame started, even if that PES packet has been
 * partially consumed.
 *
 * @param amount Number of bytes to erase from the beginning
 */
void cPtsTrackingBuffer::Erase(size_t amount) {
	if (m_data.empty() || amount == 0)
		return;

	// Only PES packets have PTS values, but not the (fragmented) frames inside.
	// The reassembled frame's PTS value will become the PTS value of the PES packet where the frame starts.
	// Therefore, always keep the PTS value for position 0 in the buffer, which is the PTS value of the PES packet where the frame starts.
	// This is normally the largest PTS value to be removed, or, if future position 0 already has a PTS value, that value will be used.
	int64_t smallestPts = AV_NOPTS_VALUE;
	auto it = m_pts.upper_bound(amount);
	if (it == m_pts.begin())
		LOGFATAL("pes: %s: %s: no PTS value found for position 0 after erasing %zu bytes", __FUNCTION__, m_identifier, amount);
	else {
		--it;  // Move to the last entry before 'amount'
		smallestPts = it->second;
	}

	std::map<size_t, int64_t> adjusted_pts;
	for (const auto& [pos, pts] : m_pts) {
		if (pos >= amount) // erase all PTS entries for data that will be removed
			adjusted_pts[pos - amount] = pts; // adjust remaining PTS entries to the new data indices
	}
	m_pts = std::move(adjusted_pts);

	m_pts[0] = smallestPts;

	m_data.erase(m_data.begin(), m_data.begin() + amount);
}

/**
 * Get the PTS value for the current buffer position
 *
 * Returns the PTS associated with position 0 in the buffer, which represents
 * the presentation timestamp for the data at the front of the buffer.
 *
 * @return PTS value, or AV_NOPTS_VALUE if no PTS is available
 */
int64_t cPtsTrackingBuffer::GetPts() {
	if (m_pts.empty())
		return AV_NOPTS_VALUE;

	return m_pts.begin()->second;
}
