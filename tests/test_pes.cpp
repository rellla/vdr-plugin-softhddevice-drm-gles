/**
 * @file test_pes.cpp
 * Unit tests for cPes class
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
std::vector<uint8_t> createBasicPesHeader(uint8_t streamId, bool withPts = false) {
    std::vector<uint8_t> data;

    // Start code prefix
    data.push_back(0x00);
    data.push_back(0x00);
    data.push_back(0x01);

    // Stream ID
    data.push_back(streamId);

    // PES packet length (0 = unspecified)
    data.push_back(0x00);
    data.push_back(0x00);

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
std::vector<uint8_t> createHevcPesPacket(bool withLeadingZero = false) {
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

TEST_CASE("cPes - Basic construction", "[pes]") {
    SECTION("Construct with valid data") {
        auto data = createBasicPesHeader(0xE0);
        cPes pes(data.data(), data.size());

        REQUIRE(pes.GetCodec() == AV_CODEC_ID_NONE);
    }

    SECTION("Construct with empty data") {
        uint8_t data[] = {};
        cPes pes(data, 0);

        REQUIRE(pes.GetCodec() == AV_CODEC_ID_NONE);
    }
}

TEST_CASE("cPes - Header validation", "[pes]") {
    SECTION("Valid PES header") {
        auto data = createBasicPesHeader(0xE0);
        cPes pes(data.data(), data.size());

        REQUIRE(pes.IsHeaderValid());
    }

    SECTION("Invalid PES header - wrong start code") {
        std::vector<uint8_t> data = {0x00, 0x00, 0x02, 0xE0, 0x00, 0x00};
        cPes pes(data.data(), data.size());

        REQUIRE(!pes.IsHeaderValid());
    }

    SECTION("Invalid PES header - missing bytes") {
        std::vector<uint8_t> data = {0x00, 0x00, 0x01};
        cPes pes(data.data(), data.size());

        REQUIRE(!pes.IsHeaderValid());
    }
}

TEST_CASE("cPes - Stream type detection", "[pes]") {
    SECTION("Video stream detection") {
        // Video streams have stream IDs 0xE0-0xEF
        for (uint8_t id = 0xE0; id <= 0xEF; id++) {
            auto data = createBasicPesHeader(id);
            cPes pes(data.data(), data.size());

            REQUIRE(pes.IsVideoStream());
            REQUIRE(!pes.IsAudioStream());
        }
    }

    SECTION("Audio stream detection") {
        // Audio streams have stream IDs 0xC0-0xCF
        for (uint8_t id = 0xC0; id <= 0xCF; id++) {
            auto data = createBasicPesHeader(id);
            cPes pes(data.data(), data.size());

            REQUIRE(pes.IsAudioStream());
            REQUIRE(!pes.IsVideoStream());
        }
    }

    SECTION("Neither audio nor video") {
        auto data = createBasicPesHeader(0xBD); // Private stream 1
        cPes pes(data.data(), data.size());

        REQUIRE(!pes.IsVideoStream());
        REQUIRE(!pes.IsAudioStream());
    }
}

TEST_CASE("cPes - MPEG2 codec detection", "[pes]") {
    SECTION("Detect MPEG2 video codec") {
        auto data = createMpeg2PesPacket();
        cPes pes(data.data(), data.size());

        pes.Parse();

        REQUIRE(pes.GetCodec() == AV_CODEC_ID_MPEG2VIDEO);
        REQUIRE(pes.IsVideoStream());
    }
}

TEST_CASE("cPes - H.264 codec detection", "[pes]") {
    SECTION("Detect H.264 codec without leading zero") {
        auto data = createH264PesPacket(false);
        cPes pes(data.data(), data.size());

        pes.Parse();

        REQUIRE(pes.GetCodec() == AV_CODEC_ID_H264);
    }

    SECTION("Detect H.264 codec with leading zero") {
        auto data = createH264PesPacket(true);
        cPes pes(data.data(), data.size());

        pes.Parse();

        REQUIRE(pes.GetCodec() == AV_CODEC_ID_H264);
    }
}

TEST_CASE("cPes - HEVC codec detection", "[pes]") {
    SECTION("Detect HEVC codec without leading zero") {
        auto data = createHevcPesPacket(false);
        cPes pes(data.data(), data.size());

        pes.Parse();

        REQUIRE(pes.GetCodec() == AV_CODEC_ID_HEVC);
    }

    SECTION("Detect HEVC codec with leading zero") {
        auto data = createHevcPesPacket(true);
        cPes pes(data.data(), data.size());

        pes.Parse();

        REQUIRE(pes.GetCodec() == AV_CODEC_ID_HEVC);
    }
}

TEST_CASE("cPes - PTS handling", "[pes]") {
    SECTION("Get PTS from packet with PTS") {
        auto data = createBasicPesHeader(0xE0, true);
        cPes pes(data.data(), data.size());

        int64_t pts = pes.GetPts();

        REQUIRE(pts == 9000);
    }

    SECTION("Get PTS from packet without PTS") {
        auto data = createBasicPesHeader(0xE0, false);
        cPes pes(data.data(), data.size());

        int64_t pts = pes.GetPts();

        REQUIRE(pts == AV_NOPTS_VALUE);
    }
}

TEST_CASE("cPes - Payload extraction", "[pes]") {
    SECTION("Get payload from MPEG2 packet") {
        auto data = createMpeg2PesPacket();
        cPes pes(data.data(), data.size());

        pes.Parse();

        const uint8_t* payload = pes.GetPayload();
        int payloadSize = pes.GetPayloadSize();

        // Verify start code is present in payload
        REQUIRE(payload != nullptr);
        REQUIRE(payload[0] == 0x00);
        REQUIRE(payload[1] == 0x00);
        REQUIRE(payload[2] == 0x01);

        REQUIRE(payloadSize == 24); // 4 bytes start code + 20 bytes dummy payload
    }

    SECTION("Get payload from H.264 packet without leading zero") {
        auto data = createH264PesPacket(false);
        cPes pes(data.data(), data.size());

        pes.Parse();

        const uint8_t* payload = pes.GetPayload();
        int payloadSize = pes.GetPayloadSize();


        // Verify start code is present in payload
        REQUIRE(payload != nullptr);
        REQUIRE(payload[0] == 0x00);
        REQUIRE(payload[1] == 0x00);
        REQUIRE(payload[2] == 0x01);

        REQUIRE(payloadSize == 25); // 4 bytes start code + 1 marker + 20 bytes dummy payload
    }

    SECTION("Get payload from H.264 packet with leading zero") {
        auto data = createH264PesPacket(true);
        cPes pes(data.data(), data.size());

        pes.Parse();

        const uint8_t* payload = pes.GetPayload();
        int payloadSize = pes.GetPayloadSize();

        // Leading zero should be skipped
        REQUIRE(payload != nullptr);
        REQUIRE(payload[0] == 0x00);
        REQUIRE(payload[1] == 0x00);
        REQUIRE(payload[2] == 0x01);

        REQUIRE(payloadSize == 25); // 4 bytes start code + 1 marker + 20 bytes dummy payload
    }

    SECTION("Payload size consistency") {
        auto data = createMpeg2PesPacket();
        cPes pes(data.data(), data.size());

        pes.Parse();

        const uint8_t* payload = pes.GetPayload();
        int payloadSize = pes.GetPayloadSize();

        // Verify payload + header size equals total size
        int headerSize = payload - data.data();
        REQUIRE(headerSize + payloadSize == static_cast<int>(data.size()));
    }
}

TEST_CASE("cPes - Audio stream handling", "[pes]") {
    SECTION("Audio stream without codec parsing") {
        auto data = createAudioPesPacket();
        cPes pes(data.data(), data.size());

        REQUIRE(pes.IsAudioStream());
        REQUIRE(!pes.IsVideoStream());

        pes.Parse();

        // TODO Audio codec detection is not implemented, yet
        REQUIRE(pes.GetCodec() == AV_CODEC_ID_NONE);
    }
}

TEST_CASE("cPes - Edge cases", "[pes]") {
    SECTION("Parse very short packet") {
        std::vector<uint8_t> data = {0x00, 0x00, 0x01, 0xE0};
        cPes pes(data.data(), data.size());

        pes.Parse();

        // Should not crash and codec should remain NONE
        REQUIRE(pes.GetCodec() == AV_CODEC_ID_NONE);
    }

    SECTION("Parse packet with no payload") {
        auto data = createBasicPesHeader(0xE0);
        cPes pes(data.data(), data.size());

        pes.Parse();

        REQUIRE(pes.GetCodec() == AV_CODEC_ID_NONE);
    }

    SECTION("Parse packet with invalid start code in payload") {
        auto data = createBasicPesHeader(0xE0);
        // Add invalid start code
        data.push_back(0x00);
        data.push_back(0x00);
        data.push_back(0x02); // Wrong start code
        data.push_back(0xB3);

        cPes pes(data.data(), data.size());

        pes.Parse();

        REQUIRE(pes.GetCodec() == AV_CODEC_ID_NONE);
    }
}

TEST_CASE("cPes - to_string utility", "[pes]") {
    SECTION("Convert unknown codec ID to string") {
        AVCodecID unknownCodec = static_cast<AVCodecID>(9999);
        REQUIRE(std::string(to_string(unknownCodec)) == "Unknown codec");
    }
}
