/**
 * @file test_pes.cpp
 * Unit tests for cPesVideo class
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

#include <catch2/catch_test_macros.hpp>
#include "../pes.h"

extern "C" {
#include <libavutil/avutil.h>
}

// Helper function to create a minimal valid PES header
// PES packet structure:
// - Start code prefix: 0x000001 (3 bytes)
// - Stream ID: 1 byte
// - PES packet length: 2 bytes
// - Optional PES header fields
std::vector<uint8_t> createBasicPesHeader(uint8_t streamId, bool withPts = false, uint16_t pesLength = 0) {
    std::vector<uint8_t> data;

    // Start code prefix
    data.push_back(0x00);
    data.push_back(0x00);
    data.push_back(0x01);

    // Stream ID
    data.push_back(streamId);

    // PES packet length (0 = unspecified)
    data.push_back((pesLength >> 8) & 0xFF);
    data.push_back(pesLength & 0xFF);

    // PES extension
    data.push_back(0x80); // '10'xxxxxx (no PES scrambling control, PES priority, data alignment indicator, copyright, original or copy)

    // PES header flags
    if (withPts) {
        data.push_back(0x80); // PTS_DTS_flags = '10' (PTS only: no ESCR flag, ES rate flag, DSM trick mode flag, additional copy info flag, PES CRC flag, PES extension flag)
        data.push_back(0x05); // PES header data length (5 bytes for PTS)

        // PTS value (5 bytes) - example: 9000 (0.1 second at 90kHz)
        data.push_back(0x21); // '0010' (marker bits) + high 3 bits of PTS + marker bit
        data.push_back(0x00);
        data.push_back(0x01); // marker bit
        data.push_back(0x46);
        data.push_back(0x51); // marker bit + low 15 bits
    } else {
        data.push_back(0x00); // No PTS/DTS, ESCR flag, ES rate flag, DSM trick mode flag, additional copy info flag, PES CRC flag, PES extension flag
        data.push_back(0x00); // PES header data length = 0
    }

    return data;
}

// Helper to create a PES packet with MPEG2 video payload
std::vector<uint8_t> createMpeg2PesPacket() {
    auto data = createBasicPesHeader(0xE0, true); // Video stream 0

    // MPEG2 video start code: 0x000001B3
    data.push_back(0x00);
    data.push_back(0x00);
    data.push_back(0x01);
    data.push_back(0xB3); // MPEG2 sequence header

    // Add some dummy payload
    for (int i = 0; i < 20; i++) {
        data.push_back(0x00);
    }

    return data;
}

// Helper to create a PES packet with H.264 video payload
std::vector<uint8_t> createH264PesPacket(bool withLeadingZero = false) {
    auto data = createBasicPesHeader(0xE0, true); // Video stream 0

    if (withLeadingZero) {
        data.push_back(0x00); // Leading zero byte
    }

    // H.264 NAL unit start code: 0x00000109
    data.push_back(0x00);
    data.push_back(0x00);
    data.push_back(0x01);
    data.push_back(0x09); // H.264 access unit delimiter
    data.push_back(0x10); // Marker byte

    // Add more data to pass array bounds check
    for (int i = 0; i < 20; i++) {
        data.push_back(0x00);
    }

    return data;
}

// Helper to create a PES packet with HEVC video payload
std::vector<uint8_t> createHevcPesVideoPacket(bool withLeadingZero = false) {
    auto data = createBasicPesHeader(0xE0, true); // Video stream 0

    if (withLeadingZero) {
        data.push_back(0x00); // Leading zero byte
    }

    // HEVC NAL unit start code: 0x00000146
    data.push_back(0x00);
    data.push_back(0x00);
    data.push_back(0x01);
    data.push_back(0x46); // HEVC access unit delimiter
    data.push_back(0x10); // Marker byte

    // Add more data to pass array bounds check
    for (int i = 0; i < 20; i++) {
        data.push_back(0x00);
    }

    return data;
}

// Helper to create a PES packet with audio payload
std::vector<uint8_t> createAudioPesPacket() {
    auto data = createBasicPesHeader(0xC0, false); // Audio stream 0

    // Add some audio payload
    for (int i = 0; i < 20; i++) {
        data.push_back(0xFF);
    }

    return data;
}

TEST_CASE("cPesVideo - Basic construction", "[pes]") {
    SECTION("Construct with valid data") {
        auto data = createBasicPesHeader(0xE0);
        cPesVideo pes(data.data(), data.size());

        REQUIRE(pes.IsValid());
    }

    SECTION("Construct with empty data") {
        uint8_t data[] = {};
        cPesVideo pes(data, 0);

        REQUIRE(!pes.IsValid());
    }
}

TEST_CASE("cPesVideo - Header validation", "[pes]") {
    SECTION("Valid PES header") {
        auto data = createBasicPesHeader(0xE0);
        cPesVideo pes(data.data(), data.size());

        REQUIRE(pes.IsValid());
    }

    SECTION("Invalid PES header - wrong start code") {
        std::vector<uint8_t> data = {0x00, 0x00, 0x02, 0xE0, 0x00, 0x00};
        cPesVideo pes(data.data(), data.size());

        REQUIRE(!pes.IsValid());
    }

    SECTION("Invalid PES header - one byte too short") {
        std::vector<uint8_t> data = {0x00, 0x00, 0x01, 0xE0, 0x00, 0x10, 0xAA, 0xBB};
        cPesVideo pes(data.data(), data.size());

        REQUIRE(!pes.IsValid());
    }

    SECTION("Stream type without PES extension") {
        std::vector<uint8_t> data;

        // Start code prefix
        data.push_back(0x00);
        data.push_back(0x00);
        data.push_back(0x01);

        // Stream ID
        data.push_back(0xBE);

        // PES packet length (0 = unspecified)
        data.push_back(0x00);
        data.push_back(0x01);

        // payload
        data.push_back(0xAA);

        cPesVideo pes(data.data(), data.size());

        REQUIRE(!pes.IsValid());
    }
}

TEST_CASE("cPesVideo - Stream type detection", "[pes]") {
    SECTION("Video stream detection") {
        // Video streams have stream IDs 0xE0-0xEF
        for (uint8_t id = 0xE0; id <= 0xEF; id++) {
            auto data = createBasicPesHeader(id);
            cPesVideo pes(data.data(), data.size());

            REQUIRE(pes.IsValid());
        }
    }

    SECTION("Audio stream detection") {
        // Audio streams have stream IDs 0xC0-0xCF
        for (uint8_t id = 0xC0; id <= 0xCF; id++) {
            auto data = createBasicPesHeader(id);
            cPesVideo pes(data.data(), data.size());

            REQUIRE(!pes.IsValid());
        }
    }

    SECTION("Neither audio nor video") {
        auto data = createBasicPesHeader(0xBD); // Private stream 1
        cPesVideo pes(data.data(), data.size());

        REQUIRE(!pes.IsValid());
    }
}


TEST_CASE("cPesVideo - PTS handling", "[pes]") {
    SECTION("Get PTS from packet with PTS") {
        auto data = createBasicPesHeader(0xE0, true);
        cPesVideo pes(data.data(), data.size());

        int64_t pts = pes.GetPts();

        REQUIRE(pts == 9000);
    }

    SECTION("Get PTS from packet without PTS") {
        auto data = createBasicPesHeader(0xE0, false);
        cPesVideo pes(data.data(), data.size());

        int64_t pts = pes.GetPts();

        REQUIRE(pts == AV_NOPTS_VALUE);
    }
}

TEST_CASE("cPesVideo - Payload extraction", "[pes]") {
    SECTION("Get payload from MPEG2 packet") {
        auto data = createMpeg2PesPacket();
        cPesVideo pes(data.data(), data.size());

        const uint8_t* payload = pes.GetPayload();
        int payloadSize = pes.GetPayloadSize();

        // Verify start code is present in payload
        REQUIRE(payload != nullptr);
        REQUIRE(payload[0] == 0x00);
        REQUIRE(payload[1] == 0x00);
        REQUIRE(payload[2] == 0x01);

        REQUIRE(payloadSize == 24); // 4 bytes start code + 20 bytes dummy payload
    }

    SECTION("Payload size consistency") {
        auto data = createMpeg2PesPacket();
        cPesVideo pes(data.data(), data.size());

        const uint8_t* payload = pes.GetPayload();
        int payloadSize = pes.GetPayloadSize();

        // Verify payload + header size equals total size
        int headerSize = payload - data.data();
        REQUIRE(headerSize + payloadSize == static_cast<int>(data.size()));
    }
}

TEST_CASE("cPesVideo - Packet length", "[pes]") {
    SECTION("Get packet length for unbounded MPEG2 (length field = 0)") {
        auto data = createMpeg2PesPacket();
        cPesVideo pes(data.data(), data.size());

        REQUIRE(pes.GetPacketLength() == static_cast<int>(data.size()));
    }

    SECTION("Get packet length for unbounded H.264 (length field = 0)") {
        auto data = createH264PesPacket(false);
        cPesVideo pes(data.data(), data.size());

        REQUIRE(pes.GetPacketLength() == static_cast<int>(data.size()));
    }

    SECTION("Get packet length with specified length field") {
        // Create a PES packet with a specific length
        // PES length field specifies bytes after the length field itself
        uint16_t pesPayloadLength = 20; // Header data (3 bytes) + actual payload
        auto data = createBasicPesHeader(0xE0, false, pesPayloadLength);

        // Add some payload to match the specified length
        for (int i = data.size() - 6; i < pesPayloadLength; i++) {
            data.push_back(0x00);
        }

        cPesVideo pes(data.data(), data.size());

        REQUIRE(pes.GetPacketLength() == 6 + pesPayloadLength);
    }

    SECTION("Get packet length for packet with PTS and specified length") {
        // PTS takes 5 bytes, so header data length = 5
        // Total PES header = 9 (fixed header) + 5 (PTS) = 14 bytes
        // If we want total packet of 50 bytes, length field = 50 - 6 = 44
        uint16_t pesPayloadLength = 44;
        auto data = createBasicPesHeader(0xE0, true, pesPayloadLength);

        // Add payload to make total packet 50 bytes
        int currentSize = data.size();
        int targetTotalSize = 6 + pesPayloadLength;
        for (int i = currentSize; i < targetTotalSize; i++) {
            data.push_back(0x00);
        }

        cPesVideo pes(data.data(), data.size());

        REQUIRE(pes.GetPacketLength() == 50);
    }

    SECTION("Get packet length when input buffer is larger than PES packet") {
        // Create a PES packet with specified length
        uint16_t pesPayloadLength = 20;
        auto data = createBasicPesHeader(0xE0, false, pesPayloadLength);

        // Add payload matching the PES length
        for (int i = data.size() - 6; i < pesPayloadLength; i++) {
            data.push_back(0xAA);
        }

        // Add extra data beyond the PES packet (simulating buffer with multiple packets)
        for (int i = 0; i < 50; i++) {
            data.push_back(0xFF);
        }

        cPesVideo pes(data.data(), data.size());

        REQUIRE(pes.GetPacketLength() == 6 + pesPayloadLength);
        REQUIRE(pes.GetPacketLength() < static_cast<int>(data.size()));
    }

    SECTION("Unbounded packet with buffer larger than actual data") {
        // Create an unbounded packet (length field = 0)
        auto data = createBasicPesHeader(0xE0, false, 0);

        // Add some actual payload
        for (int i = 0; i < 30; i++) {
            data.push_back(0xAA);
        }

        // Store the actual data size
        int actualSize = data.size();

        // Add extra buffer space (simulating oversized buffer)
        for (int i = 0; i < 50; i++) {
            data.push_back(0xFF);
        }

        cPesVideo pes(data.data(), data.size());

        REQUIRE(pes.GetPacketLength() == static_cast<int>(data.size()));
        REQUIRE(pes.GetPacketLength() > actualSize);
    }

    SECTION("Get packet length for audio packet with specified length") {
        // Audio packets typically have bounded length
        uint16_t pesPayloadLength = 30;
        auto data = createBasicPesHeader(0xC0, false, pesPayloadLength);

        // Add audio payload
        for (int i = data.size() - 6; i < pesPayloadLength; i++) {
            data.push_back(0xFF);
        }

        cPesAudio pes(data.data(), data.size());

        REQUIRE(pes.GetPacketLength() == 6 + pesPayloadLength);
    }
}

TEST_CASE("cPesAudio - Audio stream handling", "[pes]") {
    SECTION("Audio stream validation") {
        auto data = createAudioPesPacket();
        cPesAudio pes(data.data(), data.size());

        REQUIRE(pes.IsValid());
    }

    SECTION("Private stream validation (0xBD)") {
        auto data = createBasicPesHeader(0xBD, false);
        cPesAudio pes(data.data(), data.size());

        REQUIRE(pes.IsValid());
    }
}

TEST_CASE("cPesVideo - Edge cases", "[pes]") {
    SECTION("Parse very short packet") {
        std::vector<uint8_t> data = {0x00, 0x00, 0x01, 0xE0};
        cPesVideo pes(data.data(), data.size());

        // Should not crash but packet is invalid due to insufficient length
        REQUIRE(!pes.IsValid());
    }

    SECTION("Parse packet with no payload") {
        auto data = createBasicPesHeader(0xE0);
        cPesVideo pes(data.data(), data.size());

        // Packet is valid but has no payload
        REQUIRE(pes.IsValid());
    }
}

// ============================================================================
// cReassemblyBufferVideo Tests
// ============================================================================

TEST_CASE("cReassemblyBufferVideo - MPEG2 codec detection", "[reassembly][video]") {
    SECTION("Detect MPEG2 video codec") {
        cReassemblyBufferVideo buffer;

        // Create MPEG2 video frame: 0x000001B3
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x01, 0xB3, 0x00, 0x00, 0x00, 0x00};

        REQUIRE(buffer.ParseCodecHeader(fragment.data(), fragment.size()) == true);
        REQUIRE(buffer.GetCodec() == AV_CODEC_ID_MPEG2VIDEO);
    }
}

TEST_CASE("cReassemblyBufferVideo - H.264 codec detection", "[reassembly][video]") {
    SECTION("Detect H.264 without leading zero") {
        cReassemblyBufferVideo buffer;

        // H.264 NAL unit: 0x00000109
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64};

        REQUIRE(buffer.ParseCodecHeader(fragment.data(), fragment.size()) == true);
        REQUIRE(buffer.GetCodec() == AV_CODEC_ID_H264);
    }

    SECTION("Detect H.264 with leading zero") {
        cReassemblyBufferVideo buffer;

        // H.264 NAL unit with leading zero: 0x0000000109
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64};

        REQUIRE(buffer.ParseCodecHeader(fragment.data(), fragment.size()) == true);
        REQUIRE(buffer.GetCodec() == AV_CODEC_ID_H264);
    }
}

TEST_CASE("cReassemblyBufferVideo - HEVC codec detection", "[reassembly][video]") {
    SECTION("Detect HEVC without leading zero") {
        cReassemblyBufferVideo buffer;

        // HEVC NAL unit: 0x00000146
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x01, 0x46, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40};

        REQUIRE(buffer.ParseCodecHeader(fragment.data(), fragment.size()) == true);
        REQUIRE(buffer.GetCodec() == AV_CODEC_ID_HEVC);
    }

    SECTION("Detect HEVC with leading zero") {
        cReassemblyBufferVideo buffer;

        // HEVC NAL unit with leading zero: 0x0000000146
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x00, 0x01, 0x46, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40};

        REQUIRE(buffer.ParseCodecHeader(fragment.data(), fragment.size()) == true);
        REQUIRE(buffer.GetCodec() == AV_CODEC_ID_HEVC);
    }
}

TEST_CASE("cReassemblyBufferVideo - Unknown codec", "[reassembly][video]") {
    SECTION("No start code present") {
        cReassemblyBufferVideo buffer;

        std::vector<uint8_t> fragment = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

        REQUIRE(buffer.ParseCodecHeader(fragment.data(), fragment.size()) == false);
        REQUIRE(buffer.GetCodec() == AV_CODEC_ID_NONE);
    }

    SECTION("Start code present but unknown codec type") {
        cReassemblyBufferVideo buffer;

        std::vector<uint8_t> fragment = {0x00, 0x00, 0x01, 0xFF, 0x00, 0x00, 0x00, 0x00};

        REQUIRE(buffer.ParseCodecHeader(fragment.data(), fragment.size()) == false);
        REQUIRE(buffer.GetCodec() == AV_CODEC_ID_NONE);
    }
}

TEST_CASE("cReassemblyBufferVideo - HasLeadingZero detection", "[reassembly][video]") {
    SECTION("Detect leading zero with H.264") {
        cReassemblyBufferVideo buffer;

        // H.264 with leading zero: 0x00 00 00 01 09
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00};

        REQUIRE(buffer.HasLeadingZero(fragment.data(), fragment.size()) == true);
    }

    SECTION("No leading zero - normal start code") {
        cReassemblyBufferVideo buffer;

        // Normal start code: 0x00 00 01
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x01, 0x09, 0x10};

        REQUIRE(buffer.HasLeadingZero(fragment.data(), fragment.size()) == false);
    }

    SECTION("Leading zero with HEVC") {
        cReassemblyBufferVideo buffer;

        // HEVC with leading zero: 0x00 00 00 01 46
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x00, 0x01, 0x46, 0x10};

        REQUIRE(buffer.HasLeadingZero(fragment.data(), fragment.size()) == true);
    }

    SECTION("Data too short") {
        cReassemblyBufferVideo buffer;

        // Too short to have leading zero
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x00, 0x01};

        REQUIRE(buffer.HasLeadingZero(fragment.data(), fragment.size()) == false);
    }

    SECTION("First byte not zero") {
        cReassemblyBufferVideo buffer;

        // First byte not zero
        std::vector<uint8_t> fragment = {0xFF, 0x00, 0x00, 0x01, 0x09};

        REQUIRE(buffer.HasLeadingZero(fragment.data(), fragment.size()) == false);
    }
}

TEST_CASE("cReassemblyBufferVideo - Push and drain", "[reassembly][video]") {
    SECTION("Push video data and create AVPacket") {
        cReassemblyBufferVideo buffer;

        // Create MPEG2 video frame
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x01, 0xB3, 0xAA, 0xBB, 0xCC, 0xDD};

        buffer.ParseCodecHeader(fragment.data(), fragment.size());
        buffer.Push(fragment.data(), fragment.size(), 9000);

        // Drain and create AVPacket
        AVPacket *pkt = buffer.PopAvPacket();

        REQUIRE(pkt != nullptr);
        REQUIRE(pkt->pts == 9000);
        REQUIRE(pkt->size >= 8);

        av_packet_free(&pkt);
    }

    SECTION("Push H.264") {
        cReassemblyBufferVideo buffer;

        // H.264 NAL unit with leading zero: 0x0000000109
        std::vector<uint8_t> fragment = {0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64};

        buffer.ParseCodecHeader(fragment.data(), fragment.size());
        buffer.Push(fragment.data(), fragment.size(), 12000);

        AVPacket *pkt = buffer.PopAvPacket();

        REQUIRE(pkt != nullptr);
        REQUIRE(pkt->pts == 12000);
        // Should be 12 bytes: full fragment size (leading zero is kept)
        REQUIRE(pkt->size == 12);

        REQUIRE(pkt->data[0] == 0x00); // Leading zero
        REQUIRE(pkt->data[1] == 0x00);
        REQUIRE(pkt->data[2] == 0x00);
        REQUIRE(pkt->data[3] == 0x01);
        REQUIRE(pkt->data[4] == 0x09);

        av_packet_free(&pkt);
    }
}

// ============================================================================
// cReassemblyBufferAudio Tests
// ============================================================================

TEST_CASE("cReassemblyBufferAudio - MP2 codec detection", "[reassembly][audio]") {
    SECTION("Detect MP2 audio codec") {
        cReassemblyBufferAudio buffer;

        // MP2 sync word: 0xFFEx (11 bits sync + version/layer)
        // Example: 0xFFF3 (sync) + bitrate/samplerate info
        std::vector<uint8_t> fragment = { 0xFF, 0xF3, 0x44, 0xC0 }; // MP2 header

        REQUIRE(buffer.DetectCodecFromSyncWord(fragment.data(), fragment.size()) == AV_CODEC_ID_MP2);
    }
}

TEST_CASE("cReassemblyBufferAudio - AC3 codec detection", "[reassembly][audio]") {
    SECTION("Detect AC3 audio codec") {
        cReassemblyBufferAudio buffer;

        // AC3 sync word: 0x0B77
        std::vector<uint8_t> fragment = { 0x0B, 0x77, 0x00, 0x00, 0x00, 0x00 }; // AC3 header

        REQUIRE(buffer.DetectCodecFromSyncWord(fragment.data(), fragment.size()) == AV_CODEC_ID_AC3);
    }

    SECTION("Detect E-AC3 audio codec") {
        cReassemblyBufferAudio buffer;

        // E-AC3 sync word: 0x0B77 with bitstream ID > 10
        std::vector<uint8_t> fragment = { 0x0B, 0x77, 0x00, 0x00, 0x00, 0x51 }; // E-AC3 header (byte 5 > 0x50)

        REQUIRE(buffer.DetectCodecFromSyncWord(fragment.data(), fragment.size()) == AV_CODEC_ID_EAC3);
    }
}

TEST_CASE("cReassemblyBufferAudio - AAC LATM codec detection", "[reassembly][audio]") {
    SECTION("Detect AAC LATM codec") {
        cReassemblyBufferAudio buffer;

        // LATM sync word: 0x2B7 at 11 highest bits
        std::vector<uint8_t> fragment = { 0x56, 0xE0, 0x00 }; // 0x2B7 << (24-11) = 0x56E000

        REQUIRE(buffer.DetectCodecFromSyncWord(fragment.data(), fragment.size()) == AV_CODEC_ID_AAC_LATM);
    }
}

TEST_CASE("cReassemblyBufferAudio - ADTS codec detection", "[reassembly][audio]") {
    SECTION("Detect ADTS codec") {
        cReassemblyBufferAudio buffer;

        // ADTS sync word: 0xFFF
        std::vector<uint8_t> fragment = { 0xFF, 0xF1, 0x50, 0x80, 0x00, 0x1F, 0xFC }; // ADTS header

        REQUIRE(buffer.DetectCodecFromSyncWord(fragment.data(), fragment.size()) == AV_CODEC_ID_AAC);
    }
}

TEST_CASE("cReassemblyBufferAudio - Private stream handling", "[reassembly][audio]") {
    SECTION("AC3 in private stream (not audio stream)") {
        cReassemblyBufferAudio buffer;

        // AC3 can appear in private stream (0xBD) - still detected the same way
        std::vector<uint8_t> fragment = { 0x0B, 0x77, 0x00, 0x00, 0x00, 0x00 }; // AC3 header

        REQUIRE(buffer.DetectCodecFromSyncWord(fragment.data(), fragment.size()) == AV_CODEC_ID_AC3);
    }
}

TEST_CASE("cReassemblyBufferAudio - Unknown codec", "[reassembly][audio]") {
    SECTION("Garbage data returns NONE") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> fragment = { 0x00, 0x00, 0x00 };

        REQUIRE(buffer.DetectCodecFromSyncWord(fragment.data(), fragment.size()) == AV_CODEC_ID_NONE);
    }
}

// ============================================================================
// cReassemblyBufferAudio - FindSyncWord Tests
// ============================================================================

TEST_CASE("cReassemblyBufferAudio - FindSyncWord at start", "[reassembly][audio][syncword]") {
    SECTION("Find MP2 sync word at position 0") {
        cReassemblyBufferAudio buffer;

        // MP2 frame at the beginning
        std::vector<uint8_t> data = { 0xFF, 0xF3, 0x44, 0xC0 };

        SyncWordInfo result = buffer.FindSyncWord(data.data(), data.size());

        REQUIRE(result.codecId == AV_CODEC_ID_MP2);
        REQUIRE(result.pos == 0);
    }
}

TEST_CASE("cReassemblyBufferAudio - FindSyncWord with offset", "[reassembly][audio][syncword]") {
    SECTION("Find AC3 sync word at position 10") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,  // 10 bytes garbage
            0x0B, 0x77, 0x00, 0x00, 0x00, 0x00                           // AC3 sync word
        };

        SyncWordInfo result = buffer.FindSyncWord(data.data(), data.size());

        REQUIRE(result.codecId == AV_CODEC_ID_AC3);
        REQUIRE(result.pos == 10);
    }

    SECTION("Find LATM sync word in the middle") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x00, 0x00, 0x00,        // garbage
            0x56, 0xE0, 0x00,        // LATM sync word
            0xFF, 0xFF               // more data
        };

        SyncWordInfo result = buffer.FindSyncWord(data.data(), data.size());

        REQUIRE(result.codecId == AV_CODEC_ID_AAC_LATM);
        REQUIRE(result.pos == 3);
    }
}

TEST_CASE("cReassemblyBufferAudio - FindSyncWord no match", "[reassembly][audio][syncword]") {
    SECTION("No sync word in garbage data") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
        };

        SyncWordInfo result = buffer.FindSyncWord(data.data(), data.size());

        REQUIRE(result.codecId == AV_CODEC_ID_NONE);
        REQUIRE(result.pos == -1);
    }

    SECTION("Empty data") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {};

        SyncWordInfo result = buffer.FindSyncWord(data.data(), data.size());

        REQUIRE(result.codecId == AV_CODEC_ID_NONE);
        REQUIRE(result.pos == -1);
    }

    SECTION("Data too short for any codec") {
        cReassemblyBufferAudio buffer;

        // Only 2 bytes - too short for any codec (min is 3)
        std::vector<uint8_t> data = { 0xFF, 0xF1 };

        SyncWordInfo result = buffer.FindSyncWord(data.data(), data.size());

        REQUIRE(result.codecId == AV_CODEC_ID_NONE);
        REQUIRE(result.pos == -1);
    }
}

TEST_CASE("cReassemblyBufferAudio - FindSyncWord multiple candidates", "[reassembly][audio][syncword]") {
    SECTION("Returns first valid sync word when multiple present") {
        cReassemblyBufferAudio buffer;

        // AC3 sync word followed by MP2 sync word
        std::vector<uint8_t> data = {
            0x00, 0x00,                           // garbage
            0x0B, 0x77, 0x00, 0x00, 0x00, 0x00,   // AC3 sync word at position 2
            0xFF, 0xF3, 0x44, 0xC0                // MP2 sync word at position 8
        };

        SyncWordInfo result = buffer.FindSyncWord(data.data(), data.size());

        // Should find the first one (AC3)
        REQUIRE(result.codecId == AV_CODEC_ID_AC3);
        REQUIRE(result.pos == 2);
    }

    SECTION("Find sync word when partial match exists earlier") {
        cReassemblyBufferAudio buffer;

        // Partial AC3 sync (0x0B only) followed by full AC3 sync
        std::vector<uint8_t> data = {
            0x0B, 0x00,                           // partial match (not 0x0B77)
            0x0B, 0x77, 0x00, 0x00, 0x00, 0x00    // full AC3 sync word at position 2
        };

        SyncWordInfo result = buffer.FindSyncWord(data.data(), data.size());

        REQUIRE(result.codecId == AV_CODEC_ID_AC3);
        REQUIRE(result.pos == 2);
    }
}

TEST_CASE("cReassemblyBufferAudio - TruncateBufferUntilFirstValidData at start", "[reassembly][audio][consecutive]") {
    SECTION("Two consecutive LATM frames at position 0") {
        cReassemblyBufferAudio buffer;

        // First LATM frame: sync word (0x56E0) + length (5 bytes payload) = 3 + 5 = 8 bytes total
        // Second LATM frame: sync word (0x56E0) + length (3 bytes payload) = 3 + 3 = 6 bytes total
        std::vector<uint8_t> data = {
            0x56, 0xE0, 0x05, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE,  // First frame (8 bytes)
            0x56, 0xE0, 0x03, 0x11, 0x22, 0x33               // Second frame (6 bytes)
        };

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.GetSize() == 14);
        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_AAC_LATM);
    }
}

TEST_CASE("cReassemblyBufferAudio - TruncateBufferUntilFirstValidData with offset", "[reassembly][audio][consecutive]") {
    SECTION("Two consecutive LATM frames after garbage data") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x00, 0x01, 0x02, 0x03, 0x04,              // 5 bytes garbage
            0x56, 0xE0, 0x04, 0xAA, 0xBB, 0xCC, 0xDD,  // First frame at pos 5 (7 bytes)
            0x56, 0xE0, 0x02, 0x11, 0x22, 0x00         // Second frame (6 bytes)
        };

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_AAC_LATM);
        REQUIRE(buffer.GetSize() == 13);
    }

    SECTION("Two consecutive LATM frames after false positive sync word") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x56, 0xE0, 0x02, 0xAA, 0xBB,              // LATM frame (5 bytes)
            0x00, 0x00, 0x00, 0x00,                    // Garbage - no sync word at expected position
            0x56, 0xE0, 0x02, 0x11, 0x22,              // First real LATM frame at pos 9 (5 bytes)
            0x56, 0xE0, 0x03, 0x44, 0x55, 0x66         // Second real LATM frame (6 bytes)
        };

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_AAC_LATM);
        REQUIRE(buffer.GetSize() == 11);
    }
}

TEST_CASE("cReassemblyBufferAudio - TruncateBufferUntilFirstValidData edge cases", "[reassembly][audio][consecutive]") {
    SECTION("Only one LATM frame present") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x56, 0xE0, 0x05, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE  // Only one frame (8 bytes)
        };

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.GetSize() == 8);
        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_NONE);
    }

    SECTION("First frame incomplete at end of buffer") {
        cReassemblyBufferAudio buffer;

        // Frame header indicates 10 bytes payload, but only 5 bytes available
        std::vector<uint8_t> data = {
            0x56, 0xE0, 0x0A, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE  // Header says 10 bytes, but only 5 follow
        };

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.GetSize() == 8);
        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_NONE);
    }

    SECTION("Second frame incomplete") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x56, 0xE0, 0x02, 0xAA, 0xBB,  // First complete frame (5 bytes)
            0x56, 0xE0                     // Second frame header only (incomplete)
        };

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.GetSize() == 7);
        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_NONE);
    }

    SECTION("Wrong codec for second frame") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x56, 0xE0, 0x02, 0xAA, 0xBB,              // LATM frame (5 bytes)
            0x0B, 0x77, 0x00, 0x00, 0x00, 0x00         // AC3 sync word (wrong codec)
        };

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_NONE);
        REQUIRE(buffer.GetSize() == 6);
    }

    SECTION("Wrong codec for second frame, and first frame contains a sync word in payload, and second sync word is not long enough") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x56, 0xE0, 0x03, 0x56, 0xE0, 0xAA,        // LATM frame (6 bytes)
            0x0B, 0x77, 0x00, 0x00, 0x00               // AC3 sync word (wrong codec)
        };

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.GetSize() == 11);
        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_NONE);
    }

    SECTION("Only header") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x56, 0xE0, 0x02
        };

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.GetSize() == 3);
        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_NONE);
    }
}

TEST_CASE("cReassemblyBufferAudio - TruncateBufferUntilFirstValidData no sync word", "[reassembly][audio][consecutive]") {
    SECTION("No sync word in data") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
        };

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_NONE);
        REQUIRE(buffer.GetSize() == 6);
    }

    SECTION("Empty data") {
        cReassemblyBufferAudio buffer;

        std::vector<uint8_t> data = {};

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.GetSize() == 0);
        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_NONE);
    }
}

TEST_CASE("cReassemblyBufferAudio - TruncateBufferUntilFirstValidData with maximum length", "[reassembly][audio][consecutive]") {
    SECTION("LATM frame with maximum 13-bit length field") {
        cReassemblyBufferAudio buffer;

        // Maximum length in 13 bits: 0x1FFF = 8191 bytes
        // First frame: 0x56, 0xFF, 0xFF = sync word + length 0x1FFF
        // Total first frame size = 3 + 8191 = 8194 bytes
        std::vector<uint8_t> data;
        data.push_back(0x56);
        data.push_back(0xFF);
        data.push_back(0xFF);
        data.resize(8194, 0xAA);  // Fill first frame with dummy data

        // Second frame: small frame
        data.push_back(0x56);
        data.push_back(0xE0);
        data.push_back(0x01);
        data.push_back(0xBB);
        data.push_back(0x00);
        data.push_back(0x00);

        buffer.Push(data.data(), data.size(), 0);

        REQUIRE(buffer.GetSize() == data.size());
        REQUIRE(buffer.TruncateBufferUntilFirstValidData() == AV_CODEC_ID_AAC_LATM);
    }
}

// ============================================================================
// cReassemblyBufferAudio - GetFrameSize Tests
// ============================================================================

TEST_CASE("cReassemblyBufferAudio - GetFrameSize LATM codec", "[reassembly][audio][framesize]") {
    cReassemblyBufferAudio buffer;

    SECTION("LATM frame with 0 byte payload") {
        // Frame size = ((0xE0 & 0x1F) << 8) + 0x00 + 3 = 3 bytes
        std::vector<uint8_t> data = {0x56, 0xE0, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AAC_LATM, data.data());
        REQUIRE(frameSize == 3);
    }

    SECTION("LATM frame with 256 byte payload") {
        // Frame size = ((0xE1 & 0x1F) << 8) + 0x00 + 3 = 259 bytes
        std::vector<uint8_t> data = {0x56, 0xE1, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AAC_LATM, data.data());
        REQUIRE(frameSize == 259);
    }

    SECTION("LATM frame with maximum 13-bit payload (8191 bytes)") {
        // 0x1FFF = 8191, encoded as 0x56 0xFF 0xFF
        // Frame size = ((0xFF & 0x1F) << 8) + 0xFF + 3 = 8194 bytes
        std::vector<uint8_t> data = {0x56, 0xFF, 0xFF};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AAC_LATM, data.data());
        REQUIRE(frameSize == 8194);
    }
}

TEST_CASE("cReassemblyBufferAudio - GetFrameSize AAC/ADTS codec", "[reassembly][audio][framesize]") {
    cReassemblyBufferAudio buffer;

    SECTION("ADTS frame with minimum size") {
        // Frame length field spans bytes 3-5: ((data[3] & 0x03) << 11) | (data[4] << 3) | ((data[5] & 0xE0) >> 5)
        // Minimum: 0x0007 (7 bytes) = header only
        std::vector<uint8_t> data = {0xFF, 0xF1, 0x50, 0x00, 0x00, 0xE0, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AAC, data.data());
        REQUIRE(frameSize == 7);
    }

    SECTION("ADTS frame with 100 byte total size") {
        // Frame size = 100 bytes
        // Encoding: 100 = 0x0064 = 0b000 0000 0110 0100
        // data[3] = 0x00 (bits 12-11)
        // data[4] = 0x0C (bits 10-3 = 0x0C = 12)
        // data[5] = 0x80 (bits 2-0 in upper 3 bits = 100)
        std::vector<uint8_t> data = {0xFF, 0xF1, 0x50, 0x00, 0x0C, 0x80, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AAC, data.data());
        REQUIRE(frameSize == 100);
    }

    SECTION("ADTS frame with 1024 byte total size") {
        // Frame size = 1024 bytes = 0x0400 = 0b0 10000000 000
        // 13-bit field: bits 12-11=00, bits 10-3=0x80, bits 2-0=000
        // data[3] = 0x00 (bits 12-11)
        // data[4] = 0x80 (bits 10-3)
        // data[5] = 0x00 (bits 2-0)
        std::vector<uint8_t> data = {0xFF, 0xF1, 0x50, 0x00, 0x80, 0x00, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AAC, data.data());
        REQUIRE(frameSize == 1024);
    }

    SECTION("ADTS frame with maximum 13-bit size (8191 bytes)") {
        // Frame size = 8191 bytes = 0x1FFF = 0b1 1111 1111 1111
        // data[3] = 0x03 (bits 12-11 = 11)
        // data[4] = 0xFF (bits 10-3 = 11111111)
        // data[5] = 0xE0 (bits 2-0 = 111)
        std::vector<uint8_t> data = {0xFF, 0xF1, 0x50, 0x03, 0xFF, 0xE0, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AAC, data.data());
        REQUIRE(frameSize == 8191);
    }
}

TEST_CASE("cReassemblyBufferAudio - GetFrameSize AC3 codec", "[reassembly][audio][framesize]") {
    cReassemblyBufferAudio buffer;

    SECTION("AC3 frame - 48 kHz, smallest frame (128 bytes)") {
        // fscod = 01 (48 kHz), frmsizcod = 0 (first entry in table)
        // Frame size = Ac3FrameSizeTable[0][1] * 2 = 69 * 2 = 138 bytes
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x00, 0x40, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AC3, data.data());
        REQUIRE(frameSize == 138);
    }

    SECTION("AC3 frame - 44.1 kHz, smallest frame") {
        // fscod = 00 (44.1 kHz), frmsizcod = 0
        // Frame size = Ac3FrameSizeTable[0][0] * 2 = 64 * 2 = 128 bytes
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x00, 0x00, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AC3, data.data());
        REQUIRE(frameSize == 128);
    }

    SECTION("AC3 frame - 32 kHz, smallest frame") {
        // fscod = 10 (32 kHz), frmsizcod = 0
        // Frame size = Ac3FrameSizeTable[0][2] * 2 = 96 * 2 = 192 bytes
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x00, 0x80, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AC3, data.data());
        REQUIRE(frameSize == 192);
    }

    SECTION("AC3 frame - 48 kHz, largest frame (frmsizcod=37)") {
        // fscod = 01 (48 kHz), frmsizcod = 37
        // Frame size = Ac3FrameSizeTable[37][1] * 2 = 1394 * 2 = 2788 bytes
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x00, 0x65, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AC3, data.data());
        REQUIRE(frameSize == 2788);
    }

    SECTION("AC3 frame - 44.1 kHz, mid-range frame (frmsizcod=18)") {
        // fscod = 00 (44.1 kHz), frmsizcod = 18
        // Frame size = Ac3FrameSizeTable[18][0] * 2 = 320 * 2 = 640 bytes
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x00, 0x12, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_AC3, data.data());
        REQUIRE(frameSize == 640);
    }
}

TEST_CASE("cReassemblyBufferAudio - GetFrameSize AC3 error conditions", "[reassembly][audio][framesize]") {
    cReassemblyBufferAudio buffer;

    SECTION("AC3 invalid sample rate (fscod=11)") {
        // fscod = 11 (invalid), should throw
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x00, 0xC0, 0x00};
        REQUIRE_THROWS_AS(buffer.GetFrameSizeForCodec(AV_CODEC_ID_AC3, data.data()), std::invalid_argument);
    }

    SECTION("AC3 invalid frame size code (frmsizcod=38)") {
        // frmsizcod = 38 (out of range), should throw
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x00, 0x26, 0x00};
        REQUIRE_THROWS_AS(buffer.GetFrameSizeForCodec(AV_CODEC_ID_AC3, data.data()), std::invalid_argument);
    }

    SECTION("AC3 invalid frame size code (frmsizcod=63)") {
        // frmsizcod = 63 (maximum invalid), should throw
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x00, 0x3F, 0x00};
        REQUIRE_THROWS_AS(buffer.GetFrameSizeForCodec(AV_CODEC_ID_AC3, data.data()), std::invalid_argument);
    }
}

TEST_CASE("cReassemblyBufferAudio - GetFrameSize E-AC3 codec", "[reassembly][audio][framesize]") {
    cReassemblyBufferAudio buffer;

    SECTION("E-AC3 frame - minimum size (1 word = 2 bytes)") {
        // Frame size = (((data[2] & 0x07) << 8) + data[3] + 1) * 2
        // data[2] = 0x00, data[3] = 0x00 → (0 + 0 + 1) * 2 = 2 bytes
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x00, 0x00, 0x51};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_EAC3, data.data());
        REQUIRE(frameSize == 2);
    }

    SECTION("E-AC3 frame - 100 words (200 bytes)") {
        // (99 + 1) * 2 = 200 bytes
        // 99 = 0x63, data[2] = 0x00, data[3] = 0x63
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x63, 0x00, 0x51};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_EAC3, data.data());
        REQUIRE(frameSize == 200);
    }

    SECTION("E-AC3 frame - 512 words (1024 bytes)") {
        // (511 + 1) * 2 = 1024 bytes
        // 511 = 0x1FF, data[2] = 0x01, data[3] = 0xFF
        std::vector<uint8_t> data = {0x0B, 0x77, 0x01, 0xFF, 0x00, 0x51};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_EAC3, data.data());
        REQUIRE(frameSize == 1024);
    }

    SECTION("E-AC3 frame - maximum 11-bit size (2047 words = 4094 bytes)") {
        // (2046 + 1) * 2 = 4094 bytes
        // 2046 = 0x7FE, data[2] = 0x07, data[3] = 0xFE
        std::vector<uint8_t> data = {0x0B, 0x77, 0x07, 0xFE, 0x00, 0x51};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_EAC3, data.data());
        REQUIRE(frameSize == 4094);
    }
}

TEST_CASE("cReassemblyBufferAudio - GetFrameSize E-AC3 error conditions", "[reassembly][audio][framesize]") {
    cReassemblyBufferAudio buffer;

    SECTION("E-AC3 invalid fscod/fscod2 combination") {
        // data[4] & 0xF0 == 0xF0 is invalid
        std::vector<uint8_t> data = {0x0B, 0x77, 0x00, 0x00, 0xF0, 0x51};
        REQUIRE_THROWS_AS(buffer.GetFrameSizeForCodec(AV_CODEC_ID_EAC3, data.data()), std::invalid_argument);
    }
}

TEST_CASE("cReassemblyBufferAudio - GetFrameSize MP2 codec", "[reassembly][audio][framesize]") {
    cReassemblyBufferAudio buffer;

    SECTION("MP2 MPEG1 Layer 2, 128kbps, 44.1kHz, no padding") {
        // Bitrate index 8 (128k), Sample index 0 (44.1k), no padding
        // Expected: (144 * 128000) / 44100 = 417 bytes
        std::vector<uint8_t> data = {0xFF, 0xFD, 0x80, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_MP2, data.data());
        REQUIRE(frameSize == 417);
    }

    SECTION("MP2 MPEG1 Layer 2, 128kbps, 44.1kHz, with padding") {
        // Bitrate index 8 (128k), Sample index 0 (44.1k), with padding
        // Expected: (144 * 128000) / 44100 + 1 = 418 bytes
        std::vector<uint8_t> data = {0xFF, 0xFD, 0x82, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_MP2, data.data());
        REQUIRE(frameSize == 418);
    }

    SECTION("MP2 MPEG1 Layer 2, 192kbps, 48kHz, no padding") {
        // Bitrate index 10 (192k), Sample index 1 (48k), no padding
        // Expected: (144 * 192000) / 48000 = 576 bytes
        std::vector<uint8_t> data = {0xFF, 0xFD, 0xA4, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_MP2, data.data());
        REQUIRE(frameSize == 576);
    }

    SECTION("MP2 MPEG2 Layer 2, 64kbps, 24kHz, no padding") {
        // MPEG2 (bit 3 clear, bit 4 set), Bitrate index 8 (64k), Sample index 1 (24k)
        // Expected: (144 * 64000) / 24000 = 384 bytes
        std::vector<uint8_t> data = {0xFF, 0xF5, 0x84, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_MP2, data.data());
        REQUIRE(frameSize == 384);
    }

    SECTION("MP2 MPEG1 Layer 3, 128kbps, 44.1kHz, no padding") {
        // Bitrate index 9 (128k for Layer 3), Sample index 0 (44.1k)
        // Expected: (144 * 128000) / 44100 = 417 bytes
        std::vector<uint8_t> data = {0xFF, 0xFB, 0x90, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_MP2, data.data());
        REQUIRE(frameSize == 417);
    }

    SECTION("MP2 MPEG1 Layer 1, 128kbps, 44.1kHz, no padding") {
        // Bitrate index 4 (128k for Layer 1), Sample index 0 (44.1k)
        // Expected: ((12 * 128000) / 44100) * 4 = 34 * 4 = 136 bytes
        std::vector<uint8_t> data = {0xFF, 0xFF, 0x40, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_MP2, data.data());
        REQUIRE(frameSize == 136);
    }

    SECTION("MP2 MPEG1 Layer 1, 128kbps, 44.1kHz, with padding") {
        // Bitrate index 4 (128k for Layer 1), Sample index 0 (44.1k), with padding
        // Expected: ((12 * 128000) / 44100 + 1) * 4 = 35 * 4 = 140 bytes
        std::vector<uint8_t> data = {0xFF, 0xFF, 0x42, 0x00};
        int frameSize = buffer.GetFrameSizeForCodec(AV_CODEC_ID_MP2, data.data());
        REQUIRE(frameSize == 140);
    }
}

TEST_CASE("cReassemblyBufferAudio - GetFrameSize MP2 error conditions", "[reassembly][audio][framesize]") {
    cReassemblyBufferAudio buffer;

    SECTION("MP2 invalid sample rate (index 3)") {
        // Header: 0xFF 0xFD 0x5C 0x00 (sample rate index = 11)
        std::vector<uint8_t> data = {0xFF, 0xFD, 0x5C, 0x00};
        REQUIRE_THROWS_AS(buffer.GetFrameSizeForCodec(AV_CODEC_ID_MP2, data.data()), std::invalid_argument);
    }

    SECTION("MP2 invalid bit rate (index 0)") {
        // Header: 0xFF 0xFD 0x04 0x00 (bitrate index = 0, which is forbidden)
        std::vector<uint8_t> data = {0xFF, 0xFD, 0x04, 0x00};
        REQUIRE_THROWS_AS(buffer.GetFrameSizeForCodec(AV_CODEC_ID_MP2, data.data()), std::invalid_argument);
    }

    SECTION("MP2 invalid bit rate (index 15)") {
        // Header: 0xFF 0xFD 0xF0 0x00 (bitrate index = 15, which is forbidden)
        std::vector<uint8_t> data = {0xFF, 0xFD, 0xF0, 0x00};
        REQUIRE_THROWS_AS(buffer.GetFrameSizeForCodec(AV_CODEC_ID_MP2, data.data()), std::invalid_argument);
    }
}

// ============================================================================
// cPtsTrackingBuffer Tests
// ============================================================================

TEST_CASE("cPtsTrackingBuffer - Basic Push and GetPts", "[ptstracking]") {
    SECTION("Push data with PTS and retrieve it") {
        cPtsTrackingBuffer buffer("TEST");

        std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC, 0xDD};
        buffer.Push(data.data(), data.size(), 1000);

        REQUIRE(buffer.GetSize() == 4);
        REQUIRE(buffer.GetPts() == 1000);
    }

    SECTION("Push multiple data chunks with different PTS") {
        cPtsTrackingBuffer buffer("TEST");

        std::vector<uint8_t> data1 = {0xAA, 0xBB};
        std::vector<uint8_t> data2 = {0xCC, 0xDD};
        std::vector<uint8_t> data3 = {0xEE, 0xFF};

        buffer.Push(data1.data(), data1.size(), 1000);
        buffer.Push(data2.data(), data2.size(), 2000);
        buffer.Push(data3.data(), data3.size(), 3000);

        REQUIRE(buffer.GetSize() == 6);
        REQUIRE(buffer.GetPts() == 1000); // Should return first PTS
    }

    SECTION("Empty buffer returns AV_NOPTS_VALUE") {
        cPtsTrackingBuffer buffer("TEST");

        REQUIRE(buffer.GetPts() == AV_NOPTS_VALUE);
    }
}

TEST_CASE("cPtsTrackingBuffer - Erase basic functionality", "[ptstracking][erase]") {
    SECTION("Erase from buffer with single PTS entry") {
        cPtsTrackingBuffer buffer("TEST");

        // Buffer: [0-9] with PTS at position 0
        std::vector<uint8_t> data(10, 0xAA);
        buffer.Push(data.data(), data.size(), 1000);

        // Erase first 5 bytes
        buffer.Erase(5);

        REQUIRE(buffer.GetSize() == 5);
        REQUIRE(buffer.GetPts() == 1000); // PTS should be preserved at position 0
    }

    SECTION("Erase entire buffer") {
        cPtsTrackingBuffer buffer("TEST");

        std::vector<uint8_t> data(10, 0xAA);
        buffer.Push(data.data(), data.size(), 1000);

        buffer.Erase(10);

        REQUIRE(buffer.GetSize() == 0);
    }
}

TEST_CASE("cPtsTrackingBuffer - Erase with multiple PTS entries", "[ptstracking][erase]") {
    SECTION("Erase exactly at PTS boundary") {
        cPtsTrackingBuffer buffer("TEST");

        // Position 0: PTS=1000
        std::vector<uint8_t> data1(10, 0xAA);
        buffer.Push(data1.data(), data1.size(), 1000);

        // Position 10: PTS=2000
        std::vector<uint8_t> data2(10, 0xBB);
        buffer.Push(data2.data(), data2.size(), 2000);
        // Erase exactly 10 bytes (up to but not including position 10)
        buffer.Erase(10);

        REQUIRE(buffer.GetSize() == 10);
        REQUIRE(buffer.GetPts() == 2000); // Position 10 became position 0 with its PTS
    }

    SECTION("Erase removes old PTS, keeps and adjusts newer PTS") {
        cPtsTrackingBuffer buffer("TEST");

        // Position 0: PTS=1000
        std::vector<uint8_t> data1 = {0x00, 0x01, 0x02, 0x03, 0x04};
        buffer.Push(data1.data(), data1.size(), 1000);

        // Position 5: PTS=2000
        std::vector<uint8_t> data2 = {0x05, 0x06, 0x07, 0x08, 0x09};
        buffer.Push(data2.data(), data2.size(), 2000);
        // Position 10: PTS=3000
        std::vector<uint8_t> data3 = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
        buffer.Push(data3.data(), data3.size(), 3000);

        // Erase first 5 bytes (removes data with PTS 1000)
        buffer.Erase(5);

        REQUIRE(buffer.GetSize() == 10);
        REQUIRE(buffer.GetPts() == 2000); // Position 5 became position 0, keeping PTS 2000
    }

    SECTION("Erase preserves PTS when erasing before next PTS entry") {
        cPtsTrackingBuffer buffer("TEST");

        // Position 0: PTS=1000
        std::vector<uint8_t> data1(10, 0xAA);
        buffer.Push(data1.data(), data1.size(), 1000);

        // Position 10: PTS=2000
        std::vector<uint8_t> data2(10, 0xBB);
        buffer.Push(data2.data(), data2.size(), 2000);
        // Erase 7 bytes (less than position 10)
        buffer.Erase(7);

        REQUIRE(buffer.GetSize() == 13);
        REQUIRE(buffer.GetPts() == 1000); // Should preserve PTS 1000 at new position 0
    }
}

TEST_CASE("cPtsTrackingBuffer - Erase PTS inheritance logic", "[ptstracking][erase]") {
    SECTION("PTS is preserved at new position 0 when erasing between PTS entries") {
        cPtsTrackingBuffer buffer("TEST");

        // Position 0: PTS=1000
        std::vector<uint8_t> data1(10, 0xAA);
        buffer.Push(data1.data(), data1.size(), 1000);

        // Position 10: PTS=2000
        std::vector<uint8_t> data2(10, 0xBB);
        buffer.Push(data2.data(), data2.size(), 2000);
        // Position 20: PTS=3000
        std::vector<uint8_t> data3(10, 0xCC);
        buffer.Push(data3.data(), data3.size(), 3000);

        // Erase 12 bytes (removes position 0 and part of data before position 10)
        // After erase: data from position 12 onwards remains
        // Position 12 had no PTS entry, should inherit from largest removed PTS (2000)
        buffer.Erase(12);

        REQUIRE(buffer.GetSize() == 18);
        REQUIRE(buffer.GetPts() == 2000);
    }
}

TEST_CASE("cPtsTrackingBuffer - Erase with fragmented frames", "[ptstracking][erase]") {
    SECTION("Simulates removing partial frame while preserving PTS") {
        cPtsTrackingBuffer buffer("TEST");

        // Frame 1 (complete): position 0-49, PTS=1000
        std::vector<uint8_t> frame1(50, 0xAA);
        buffer.Push(frame1.data(), frame1.size(), 1000);

        // Frame 2 (complete): position 50-149, PTS=2000
        std::vector<uint8_t> frame2(100, 0xBB);
        buffer.Push(frame2.data(), frame2.size(), 2000);
        // Frame 3 (partial): position 150-199, PTS=3000
        std::vector<uint8_t> frame3_part(50, 0xCC);
        buffer.Push(frame3_part.data(), frame3_part.size(), 3000);

        // Drain frame 1 and part of frame 2
        buffer.Erase(80);

        REQUIRE(buffer.GetSize() == 120);
        // Remaining data (partial frame 2 + frame 3) should have PTS from frame 2's start
        REQUIRE(buffer.GetPts() == 2000);
    }

    SECTION("Multiple erases progressively consume data") {
        cPtsTrackingBuffer buffer("TEST");

        // Add data with PTS entries
        std::vector<uint8_t> data1(10, 0xAA);
        buffer.Push(data1.data(), data1.size(), 1000);

        std::vector<uint8_t> data2(10, 0xBB);
        buffer.Push(data2.data(), data2.size(), 2000);

        std::vector<uint8_t> data3(10, 0xCC);
        buffer.Push(data3.data(), data3.size(), 3000);

        // First erase
        buffer.Erase(5);
        REQUIRE(buffer.GetSize() == 25);
        REQUIRE(buffer.GetPts() == 1000);

        // Second erase
        buffer.Erase(8);
        REQUIRE(buffer.GetSize() == 17);
        REQUIRE(buffer.GetPts() == 2000);

        // Third erase
        buffer.Erase(12);
        REQUIRE(buffer.GetSize() == 5);
        REQUIRE(buffer.GetPts() == 3000);
    }
}

TEST_CASE("cPtsTrackingBuffer - Erase edge cases", "[ptstracking][erase]") {
    SECTION("Erase 0 bytes does nothing") {
        cPtsTrackingBuffer buffer("TEST");

        std::vector<uint8_t> data(10, 0xAA);
        buffer.Push(data.data(), data.size(), 1000);

        buffer.Erase(0);

        REQUIRE(buffer.GetSize() == 10);
        REQUIRE(buffer.GetPts() == 1000);
    }

    SECTION("Erase single byte") {
        cPtsTrackingBuffer buffer("TEST");

        std::vector<uint8_t> data = {0xAA};
        buffer.Push(data.data(), data.size(), 1000);

        std::vector<uint8_t> data2 = {0xBB};
        buffer.Push(data2.data(), data2.size(), 2000);

        buffer.Erase(1);

        REQUIRE(buffer.GetSize() == 1);
        REQUIRE(buffer.GetPts() == 2000);
    }
}

TEST_CASE("cPtsTrackingBuffer - Complex scenarios", "[ptstracking][erase]") {
    SECTION("Interleaved push and erase operations") {
        cPtsTrackingBuffer buffer("TEST");

        // Initial data
        std::vector<uint8_t> data1(20, 0xAA);
        buffer.Push(data1.data(), data1.size(), 1000);

        // Erase some
        buffer.Erase(5);
        REQUIRE(buffer.GetSize() == 15);
        REQUIRE(buffer.GetPts() == 1000);

        // Add more data
        std::vector<uint8_t> data2(10, 0xBB);
        buffer.Push(data2.data(), data2.size(), 2000);
        REQUIRE(buffer.GetSize() == 25);

        // Erase across PTS boundary
        buffer.Erase(18);
        REQUIRE(buffer.GetSize() == 7);
        REQUIRE(buffer.GetPts() == 2000);
    }

    SECTION("Three PTS entries, erase middle one") {
        cPtsTrackingBuffer buffer("TEST");

        // Position 0: PTS=1000
        std::vector<uint8_t> data1(10, 0xAA);
        buffer.Push(data1.data(), data1.size(), 1000);

        // Position 10: PTS=2000
        std::vector<uint8_t> data2(10, 0xBB);
        buffer.Push(data2.data(), data2.size(), 2000);
        // Position 20: PTS=3000
        std::vector<uint8_t> data3(10, 0xCC);
        buffer.Push(data3.data(), data3.size(), 3000);

        // Erase 15 bytes (removes first PTS, middle of second chunk)
        buffer.Erase(15);

        REQUIRE(buffer.GetSize() == 15);
        REQUIRE(buffer.GetPts() == 2000); // Should inherit from position 10
    }

    SECTION("Large buffer with many PTS entries") {
        cPtsTrackingBuffer buffer("TEST");

        // Add 10 chunks of 100 bytes each with different PTS
        for (int i = 0; i < 10; i++) {
            std::vector<uint8_t> data(100, static_cast<uint8_t>(i));
            buffer.Push(data.data(), data.size(), 1000 * (i + 1));
        }

        REQUIRE(buffer.GetSize() == 1000);
        REQUIRE(buffer.GetPts() == 1000);

        // Erase 450 bytes (removes first 4 chunks and half of 5th)
        buffer.Erase(450);

        REQUIRE(buffer.GetSize() == 550);
        REQUIRE(buffer.GetPts() == 5000); // Should be from 5th chunk
    }
}

TEST_CASE("cPtsTrackingBuffer - Reset functionality", "[ptstracking]") {
    SECTION("Reset clears all data and PTS") {
        cPtsTrackingBuffer buffer("TEST");

        std::vector<uint8_t> data1(10, 0xAA);
        buffer.Push(data1.data(), data1.size(), 1000);

        std::vector<uint8_t> data2(10, 0xBB);
        buffer.Push(data2.data(), data2.size(), 2000);

        buffer.Reset();

        REQUIRE(buffer.GetSize() == 0);
        REQUIRE(buffer.GetPts() == AV_NOPTS_VALUE);
    }
}
