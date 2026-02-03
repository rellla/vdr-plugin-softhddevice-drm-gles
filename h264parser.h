// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file h264parser.h
 * H.264 Parser Header File
 *
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __H264PARSER_H
#define __H264PARSER_H

#include <set>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

/**
 * @addtogroup misc
 * @{
 */

typedef enum {
	NALU_TYPE_NON_IDR = (1 << 0),
	NALU_TYPE_PART_A  = (1 << 1),
	NALU_TYPE_PART_B  = (1 << 2),
	NALU_TYPE_PART_C  = (1 << 3),
	NALU_TYPE_IDR     = (1 << 4),
	NALU_TYPE_SEI     = (1 << 5),
	NALU_TYPE_SPS     = (1 << 6),
	NALU_TYPE_PPS     = (1 << 7),
	NALU_TYPE_AUD     = (1 << 8)
} NalUnitTypes;

/**
 * H.264 Parser
 */
class cH264Parser {
private:
	struct RefPicMod {
		int list;
		int idc;
		int abs_diff_pic_num_minus1;
		int long_term_pic_num;
	};

public:
	cH264Parser(AVPacket *, int, int, int);
	void BuildInvalidReferenceString(int);
	void BuildValidReferenceString(void);
	void AddFrameNumber(int);
	void AddInvalidReference(int, int);
	void AddValidReference(int);
	void PrintNalUnits(void);
	void PrintStreamData(void);

	std::string GetNalUnitString(void) { return m_naluString; };
	int GetWidth(void) { return m_width; };
	int GetHeight(void) { return m_height; };
	bool IsMbaff(void) { return m_mbaff; };
	bool HasSPS(void) { return m_hasSPS; };
	bool HasPPS(void) { return m_hasPPS; };
	bool IsPSlice() const { return m_sliceType == 0; }
	bool IsBSlice() const { return m_sliceType == 1; }
	bool IsISlice() const { return m_sliceType == 2; }
	bool IsIDR() const { return m_isIDR; }
	bool IsReference() const { return m_isReference; }
	int GetFrameNum() const { return m_frameNum; }
	const std::vector<RefPicMod>& GetRefMods() const { return m_refMods; }
	int GetLog2MaxFrameNumMinus4() const { return m_log2MaxFrameNumMinus4; }
	int GetPpsNumRefIdxL0DefaultActiveMinus1(void) { return m_ppsNumRefIdxL0DefaultActiveMinus1; };
	int GetPpsNumRefIdxL1DefaultActiveMinus1(void) { return m_ppsNumRefIdxL1DefaultActiveMinus1; };
	int GetNumRefIdxL0Active(void) { return m_numRefIdxL0Active; };
	int GetNumRefIdxL1Active(void) { return m_numRefIdxL1Active; };
	bool HasInvalidReferences(void) { return m_hasInvalidReferences; };
	bool HasInvalidBackwardReferences(void) { return m_hasInvalidBackwardReferences; };
	bool HasParseError(void) const { return m_parseError; };

private:
	AVPacket *m_pAvpkt;
	const unsigned char *m_pStart;
	std::vector<uint8_t> m_rbsp;
	unsigned short m_nLength;
	int m_nCurrentBit;

	int m_nalutype = 0;
	int m_width = 0;
	int m_height = 0;
	bool m_hasSPS = false;
	bool m_hasPPS = false;
	bool m_isIDR = false;
	bool m_isReference = false;
	bool m_mbaff = false;
	bool m_parseError = false;

	std::string m_naluString;

	int m_sliceType = -1;      // normalized: 0=P, 2=I
	int m_frameNum = -1;
	int m_nalRefIdc = 0;
	std::set<int> m_invalidReferences;
	std::set<int> m_validReferences;
	std::vector<RefPicMod> m_refMods;
	bool m_hasInvalidReferences = false;
	bool m_hasInvalidBackwardReferences = false;
	bool m_hasValidReferences = false;

	int m_log2MaxFrameNumMinus4 = -4;             // saved from SPS
	int m_ppsNumRefIdxL0DefaultActiveMinus1 = -1; // saved from PPS
	int m_ppsNumRefIdxL1DefaultActiveMinus1 = -1; // saved from PPS
	int m_numRefIdxL0Active;
	int m_numRefIdxL1Active;

	unsigned int ReadBit(void);
	unsigned int ReadBits(int);
	unsigned int ReadExponentialGolombCode(void);
	unsigned int ReadSE(void);
	int GetSPSOffset(void);
	int GetPPSOffset(void);
	int GetSliceOffset(void);
	void ConvertEBSPtoRBSP(const uint8_t *, int);
};

/** @} */

#endif
