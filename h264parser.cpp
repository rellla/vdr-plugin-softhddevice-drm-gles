/**
 * @file h264parser.cpp
 * H264 parser class
 *
 * This file defines cH264Parser which is used to parse
 * width and height from a H264 stream.
 *
 * @copyright (c) 2018 - 2019 by zille.  All Rights Reserved.
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

#include <cassert>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "h264parser.h"
#include "logger.h"
#include "misc.h"

/*****************************************************************************
 * cH264Parser class
 ****************************************************************************/

/**
 * Returns true, if we have a 0x000001 or 0x00000001 start code
 * in data at the offset position
 */
bool isValidStartCode(const uint8_t *data, int offset, int size, int &startCodeLength)
{
	if (offset + 4 <= size && ReadBytes(&data[offset], 4) == 0x00000001) {
		startCodeLength = 4;
		return true;
	}

	if (offset + 3 <= size && ReadBytes(&data[offset], 3) == 0x000001) {
		startCodeLength = 3;
		return true;
	}

	return false;
}

/**
 * Return the nal unit type
 */
static int NalUnitType(const uint8_t *data, int offset, int startCodeLength)
{
	return data[offset + startCodeLength] & 0x1F;
}

int cH264Parser::GetSPSOffset(void)
{
	int offset = -1;

	for (int i = 0; i < m_pAvpkt->size; i++) {
		int startCodeLength = 0;

		if (!isValidStartCode(m_pAvpkt->data, i, m_pAvpkt->size, startCodeLength))
			continue;

		if (NalUnitType(m_pAvpkt->data, i, startCodeLength) == 7) {
			offset = i;
			break;
		}
	}

	return offset;
}

int cH264Parser::GetPPSOffset(void)
{
	int offset = -1;

	for (int i = 0; i < m_pAvpkt->size; i++) {
		int startCodeLength = 0;

		if (!isValidStartCode(m_pAvpkt->data, i, m_pAvpkt->size, startCodeLength))
			continue;

		if (NalUnitType(m_pAvpkt->data, i, startCodeLength) == 8) {
			offset = i;
			break;
		}
	}

	return offset;
}

int cH264Parser::GetSliceOffset(void)
{
	int offset = -1;

	for (int i = 0; i < m_pAvpkt->size; i++) {
		int startCodeLength = 0;

		if (!isValidStartCode(m_pAvpkt->data, i, m_pAvpkt->size, startCodeLength))
			continue;

		if (NalUnitType(m_pAvpkt->data, i, startCodeLength) == 1 || NalUnitType(m_pAvpkt->data, i, startCodeLength) == 5) {
			offset = i;
			break;
		}
	}

	return offset;
}

/**
 * Init the h264 parser and detect the nalu types
 *
 * @param avpkt      AVPacket to parse
 */
cH264Parser::cH264Parser(AVPacket *avpkt, int maxFrameNum, int refIdxL0, int refIdxL1)
	: m_pAvpkt(avpkt),
	  m_log2MaxFrameNumMinus4(maxFrameNum),
	  m_ppsNumRefIdxL0DefaultActiveMinus1(refIdxL0),
	  m_ppsNumRefIdxL1DefaultActiveMinus1(refIdxL1)
{
	int i;

	// part 1: collect the nalu types in the packet
	for (i = 0; i < m_pAvpkt->size; i++) {
		int startCodeLength = 0;

		if (!isValidStartCode(m_pAvpkt->data, i, m_pAvpkt->size, startCodeLength))
			continue;

		int naluType = NalUnitType(m_pAvpkt->data, i, startCodeLength);
		switch (naluType) {
			case 1: m_naluString += " NON-IDR"; m_nalutype |= NALU_TYPE_NON_IDR; break;
			case 2: m_naluString += " PART_A";  m_nalutype |= NALU_TYPE_PART_A;  break;
			case 3: m_naluString += " PART_B";  m_nalutype |= NALU_TYPE_PART_B;  break;
			case 4: m_naluString += " PART_C";  m_nalutype |= NALU_TYPE_PART_C;  break;
			case 5: m_naluString += " IDR";     m_nalutype |= NALU_TYPE_IDR;     break;
			case 6: m_naluString += " SEI";     m_nalutype |= NALU_TYPE_SEI;     break;
			case 7: m_naluString += " SPS";     m_nalutype |= NALU_TYPE_SPS;     break;
			case 8: m_naluString += " PPS";     m_nalutype |= NALU_TYPE_PPS;     break;
			case 9: m_naluString += " AUD";     m_nalutype |= NALU_TYPE_AUD;     break;
			default: break;
		}

		i += startCodeLength - 1;
	}

	// part 2: parse h264 SPS and get width and height
	int spsOffset = GetSPSOffset();
	m_parseError = false;

	// SPS is available
	if (spsOffset != -1) {
		m_hasSPS = true;
		int startCodeLength = 0;
		isValidStartCode(m_pAvpkt->data, spsOffset, m_pAvpkt->size, startCodeLength);

		const uint8_t *nalPayload = &m_pAvpkt->data[spsOffset + startCodeLength + 1];
		int nalLength = m_pAvpkt->size - spsOffset - startCodeLength -1;

		ConvertEBSPtoRBSP(nalPayload, nalLength);
		m_pStart = m_rbsp.data();
		m_nLength = m_rbsp.size();

		m_nCurrentBit = 0;

		int frameCropLeftOffset = 0;
		int frameCropRightOffset = 0;
		int frameCropTopOffset = 0;
		int frameCropBottomOffset = 0;
		int chromaFormatIdc = 0;
		int separateColorPlaneFlag = 0;

		int profileIdc = ReadBits(8);
		ReadBits(16);
		ReadExponentialGolombCode();

		if (profileIdc == 100 || profileIdc == 110 ||
		    profileIdc == 122 || profileIdc == 244 ||
		    profileIdc == 44 || profileIdc == 83 ||
		    profileIdc == 86 || profileIdc == 118) {

			chromaFormatIdc = ReadExponentialGolombCode();
			if (chromaFormatIdc == 3)
				separateColorPlaneFlag = ReadBit();
			ReadExponentialGolombCode();
			ReadExponentialGolombCode();
			ReadBit();
			int seqScalingMatrixPresentFlag = ReadBit();
			if (seqScalingMatrixPresentFlag) {
				for (int i = 0; i < 8; i++) {
					int seqScalingListPresentFlag = ReadBit();
					if (seqScalingListPresentFlag) {
						int sizeOfScalingList = (i < 6) ? 16 : 64;
						int lastScale = 8;
						int nextScale = 8;
						for (int j = 0; j < sizeOfScalingList; j++) {
							if (nextScale != 0) {
								int delta_scale = ReadSE();
								nextScale = (lastScale + delta_scale + 256) % 256;
							}
							lastScale = (nextScale == 0) ? lastScale : nextScale;
						}
					}
				}
			}
		}
		m_log2MaxFrameNumMinus4 = ReadExponentialGolombCode();
		int picOrderCntType = ReadExponentialGolombCode();
		if (picOrderCntType == 0) {
			ReadExponentialGolombCode();
		} else if (picOrderCntType == 1) {
			ReadBit();
			ReadSE();
			ReadSE();
			int numRefFramesInPicOrderCntCycle = ReadExponentialGolombCode();
			for (int i = 0; i < numRefFramesInPicOrderCntCycle; i++ ) {
				ReadSE();
			}
		}
		ReadExponentialGolombCode();
		ReadBit();
		int picWidthInMbsMinusOne = ReadExponentialGolombCode();
		int picHeightInMapUnitsMinusOne = ReadExponentialGolombCode();
		int frameMbsOnlyFlag = ReadBit();
		if (!frameMbsOnlyFlag) {
			m_mbaff = ReadBit();
		}

		ReadBit();
		int frameCroppingFlag = ReadBit();
		if (frameCroppingFlag) {
			frameCropLeftOffset = ReadExponentialGolombCode();
			frameCropRightOffset = ReadExponentialGolombCode();
			frameCropTopOffset = ReadExponentialGolombCode();
			frameCropBottomOffset = ReadExponentialGolombCode();
		}

		int subWidthC = 0;
		int subHeightC = 0;

		if (chromaFormatIdc == 0 && separateColorPlaneFlag == 0) { // monochrome
			subWidthC = subHeightC = 2;
		} else if (chromaFormatIdc == 1 && separateColorPlaneFlag == 0) { // 4:2:0
			subWidthC = subHeightC = 2;
		} else if (chromaFormatIdc == 2 && separateColorPlaneFlag == 0) { // 4:2:2
			subWidthC = 2;
			subHeightC = 1;
		} else if (chromaFormatIdc == 3) { // 4:4:4
			if (separateColorPlaneFlag == 0) {
				subWidthC = subHeightC = 1;
			} else if (separateColorPlaneFlag == 1) {
				subWidthC = subHeightC = 0;
			}
		}

		m_width = ((picWidthInMbsMinusOne + 1) * 16) -
			subWidthC * (frameCropRightOffset + frameCropLeftOffset);

		m_height = ((2 - frameMbsOnlyFlag)* (picHeightInMapUnitsMinusOne +1) * 16) -
			subHeightC * ((frameCropBottomOffset * 2) + (frameCropTopOffset * 2));

//		if (m_parseError)
//			LOGWARNING("SPS parsing error");
	}

	// part 3: parse h264 PPS
	int ppsOffset = GetPPSOffset();
	m_parseError = false;

	// PPS is available
	if (ppsOffset != -1) {
		m_hasPPS = true;

		int startCodeLength = 0;
		isValidStartCode(m_pAvpkt->data, ppsOffset, m_pAvpkt->size, startCodeLength);

		const uint8_t *nalPayload = &m_pAvpkt->data[ppsOffset + startCodeLength + 1];
		int nalLength = m_pAvpkt->size - ppsOffset - startCodeLength -1;

		ConvertEBSPtoRBSP(nalPayload, nalLength);

		m_pStart = m_rbsp.data();
		m_nLength = m_rbsp.size();
		m_nCurrentBit = 0;

		ReadExponentialGolombCode(); // PicParameterSetId
		ReadExponentialGolombCode(); // SeqParameterSetId

		ReadBit(); // entropy_coding_mode_flag
		ReadBit(); // bottom_field_pic_order_in_frame_present_flag

		int num_slice_groups_minus1 = ReadExponentialGolombCode();
		if (num_slice_groups_minus1 > 0) {
			int slice_group_map_type = ReadExponentialGolombCode();

			if (slice_group_map_type == 0) {
				for (int i = 0; i <= num_slice_groups_minus1; i++)
					ReadExponentialGolombCode(); // run_length_minus1
			} else if (slice_group_map_type == 2) {
				for (int i = 0; i < num_slice_groups_minus1; i++) {
					ReadExponentialGolombCode(); // top_left
					ReadExponentialGolombCode(); // bottom_right
				}
			} else if (slice_group_map_type == 3 ||
			           slice_group_map_type == 4 ||
			           slice_group_map_type == 5) {
				ReadBit();                    // slice_group_change_direction_flag
				ReadExponentialGolombCode();  // slice_group_change_rate_minus1
			} else if (slice_group_map_type == 6) {
				int pic_size_in_map_units_minus1 = ReadExponentialGolombCode();

				int bits = 0;
				while ((1 << bits) < (num_slice_groups_minus1 + 1)) {
					bits++;
				}

				for (int i = 0; i <= pic_size_in_map_units_minus1; i++) {
					for (int b = 0; b < bits; b++) {
						ReadBit(); // slice_group_id
					}
				}
			}
		}

		m_ppsNumRefIdxL0DefaultActiveMinus1 = ReadExponentialGolombCode();
		m_ppsNumRefIdxL1DefaultActiveMinus1 = ReadExponentialGolombCode();

		m_numRefIdxL0Active = m_ppsNumRefIdxL0DefaultActiveMinus1 + 1;
		m_numRefIdxL1Active = m_ppsNumRefIdxL1DefaultActiveMinus1 + 1;

//		if (m_parseError)
//			LOGWARNING("PPS parsing error");
	}

	// part 4: parse slice header
	int sliceOffset = GetSliceOffset();
	m_parseError = false;

	// slice is available
	if (sliceOffset != -1) {
		int startCodeLength = 0;
		isValidStartCode(m_pAvpkt->data, sliceOffset, m_pAvpkt->size, startCodeLength);

		const uint8_t *nalPayload = &m_pAvpkt->data[sliceOffset + startCodeLength + 1];
		int nalLength = m_pAvpkt->size - sliceOffset - startCodeLength - 1;

		ConvertEBSPtoRBSP(nalPayload, nalLength);

		m_pStart = m_rbsp.data();
		m_nLength = m_rbsp.size();

		uint8_t nalHeader = m_pAvpkt->data[sliceOffset + startCodeLength];
		m_nalRefIdc = (nalHeader >> 5) & 0x03;
		m_isReference = (m_nalRefIdc != 0);
		m_isIDR = ((nalHeader & 0x1F) == 5);

		m_nCurrentBit = 0;

		ReadExponentialGolombCode(); // int first_mb_in_slice =
		int slice_type_raw = ReadExponentialGolombCode();

		m_sliceType = slice_type_raw % 5;   // normalize

		ReadExponentialGolombCode(); // int pic_parameter_set_id =

		int frame_num_bits = m_log2MaxFrameNumMinus4 + 4;
		m_frameNum = ReadBits(frame_num_bits);
//		if (m_parseError) {
//			LOGWARNING("Slice parsing error -> frameNum");
//			return;
//		}

		if (m_isIDR)
			ReadExponentialGolombCode(); // idr_pic_id

		m_refMods.clear();
		bool refListModFlagL0;
		bool refListModFlagL1;

		if (m_sliceType == 0) { // P-slice
			m_naluString += "         -P-    ";

			int num_ref_idx_override = ReadBit();

			if (num_ref_idx_override)
				m_numRefIdxL0Active = ReadExponentialGolombCode() + 1;
			 else
				m_numRefIdxL0Active = m_ppsNumRefIdxL0DefaultActiveMinus1 + 1;

			refListModFlagL0 = ReadBit();

			if (refListModFlagL0) {
				int idc;
				do {
					idc = ReadExponentialGolombCode();

					if (idc == 0 || idc == 1) {
						RefPicMod mod {};
						mod.list = 0;
						mod.idc = idc;
						mod.abs_diff_pic_num_minus1 = ReadExponentialGolombCode();
						m_refMods.push_back(mod);
					// ignore long-term ?
					/*
					} else if (idc == 2) {
						RefPicMod mod {};
						mod.list = 0;
						mod.idc = idc;
						mod.long_term_pic_num = ReadExponentialGolombCode();
						m_refMods.push_back(mod);
					*/
					}
				} while (idc != 3);
			}
		} else if (m_sliceType == 1) { // B-slice
			m_naluString += "            -B- ";

			int num_ref_idx_override = ReadBit();

			if (num_ref_idx_override) {
				m_numRefIdxL0Active = ReadExponentialGolombCode() + 1;
				m_numRefIdxL1Active = ReadExponentialGolombCode() + 1;
			} else {
				m_numRefIdxL0Active = m_ppsNumRefIdxL0DefaultActiveMinus1 + 1;
				m_numRefIdxL1Active = m_ppsNumRefIdxL1DefaultActiveMinus1 + 1;
			}

			// ----- List 0 -----
			refListModFlagL0 = ReadBit();

			if (refListModFlagL0) {
				int idc;
				do {
					idc = ReadExponentialGolombCode();
					if (idc == 0 || idc == 1) {
						RefPicMod mod {};
						mod.list = 0;
						mod.idc = idc;
						mod.abs_diff_pic_num_minus1 = ReadExponentialGolombCode();
						m_refMods.push_back(mod);
					// ignore long-term ?
					/*
					} else if (idc == 2) {
						RefPicMod mod {};
						mod.list = 0;
						mod.idc = idc;
						mod.long_term_pic_num = ReadExponentialGolombCode();
						m_refMods.push_back(mod);
					*/
					}
				} while (idc != 3);
			}

			// ----- List 1 -----
			refListModFlagL1 = ReadBit();

			if (refListModFlagL1) {
				int idc;
				do {
					idc = ReadExponentialGolombCode();
					if (idc == 0 || idc == 1) {
						RefPicMod mod {};
						mod.list = 1;
						mod.idc = idc;
						mod.abs_diff_pic_num_minus1 = ReadExponentialGolombCode();
						m_refMods.push_back(mod);
					// ignore long-term ?
					/*
					} else if (idc == 2) {
						RefPicMod mod {};
						mod.list = 1;
						mod.idc = idc;
						mod.long_term_pic_num = ReadExponentialGolombCode();
						m_refMods.push_back(mod);
					*/
					}
				} while (idc != 3);
			}
		} else if (m_sliceType == 2) { // I-slice
			m_naluString += " -I-    ";
		} else if (m_sliceType == 3) { // SP-slice
			m_naluString += "       -SP-   ";
		} else if (m_sliceType == 4) { // SI-slice
			m_naluString += " -SI-   ";
		}
//		if (m_parseError)
//			LOGWARNING("Slice parsing error");
	}
}

void cH264Parser::AddInvalidReference(int modRef, int frameNumber)
{
	m_invalidReferences.insert(modRef);
	m_hasInvalidReferences = true;

	if (modRef < frameNumber)
		m_hasInvalidBackwardReferences = true;
}

void cH264Parser::AddValidReference(int modRef)
{
	m_validReferences.insert(modRef);
	m_hasValidReferences = true;
}

void cH264Parser::BuildInvalidReferenceString(int frameNumber)
{
	if (!m_hasInvalidReferences)
		return;

	m_naluString += " !!!";
	for (auto r : m_invalidReferences) {
		if (r < frameNumber) {
			m_naluString += " ";
			m_naluString += std::to_string(r);
		}
	}
}

void cH264Parser::BuildValidReferenceString(void)
{
	if (!m_hasValidReferences)
		return;

	m_naluString += " -->";
	for (auto r : m_validReferences) {
		m_naluString += " ";
		m_naluString += std::to_string(r);
	}
}

void cH264Parser::AddFrameNumber(int num)
{
	if (num != -1) {
		if (num < 10)
			m_naluString += "#  ";
		else if (num < 100)
			m_naluString += "# ";
		else
			m_naluString += "#";
		m_naluString += std::to_string(num);
	} else {
		m_naluString += "    ";
	}
}

/**
 * Print raw stream data of the first 35 bytes
 */
void cH264Parser::PrintStreamData(void)
{
	const uint8_t *data = m_pAvpkt->data;

	LOGDEBUG("Stream: %02x %02x %02x %02x %02x %02x %02x %02x %02x "
	         "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
	         "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x size %d",
	         data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8],
	         data[9], data[10], data[11], data[12], data[13], data[14], data[15], data[16], data[17],
	         data[18], data[19], data[20], data[21], data[22], data[23], data[24], data[25], data[26],
	         data[27], data[28], data[29], data[30], data[31], data[32], data[33], data[34], m_pAvpkt->size);
}

void cH264Parser::PrintNalUnits(void)
{
	LOGDEBUG2(L_CODEC, "H264Parser: %s %s (%d x %d)", __FUNCTION__,
		m_naluString.c_str(), m_width, m_height);
}

/*
 * helper functions to parse resolution from stream
 */
unsigned int cH264Parser::ReadBit()
{
	if (m_nCurrentBit >= m_nLength * 8) {
		m_parseError = true;
		return 0;
	}

	int nIndex  = m_nCurrentBit / 8;
	int nOffset = m_nCurrentBit % 8;

	m_nCurrentBit++;
	return (m_pStart[nIndex] >> (7 - nOffset)) & 0x01;
}

unsigned int cH264Parser::ReadBits(int n)
{
	if (m_nCurrentBit + n > m_nLength * 8) {
		m_parseError = true;
		return 0;
	}

	unsigned int r = 0;

	for (int i = 0; i < n; i++) {
		r = (r << 1) | ReadBit();
	}
	return r;
}

unsigned int cH264Parser::ReadExponentialGolombCode()
{
	int zeros = 0;

	while (zeros < 32) {
	if (m_nCurrentBit >= m_nLength * 8) {
		m_parseError = true;
		return 0;
	}

	if (ReadBit() == 0)
		zeros++;
	else
		break;
	}

	if (zeros == 32) {
		m_parseError = true;
		return 0;
	}

	unsigned int suffix = 0;
	if (zeros > 0)
		suffix = ReadBits(zeros);

	return ((1u << zeros) - 1) + suffix;
}

unsigned int cH264Parser::ReadSE()
{
	int r = ReadExponentialGolombCode();

	if (r & 0x01) {
		r = (r+1)/2;
	} else {
		r = -(r/2);
	}
	return r;
}

void cH264Parser::ConvertEBSPtoRBSP(const uint8_t *src, int length)
{
	m_rbsp.clear();
	m_rbsp.reserve(length);

	int zeroCount = 0;

	for (int i = 0; i < length; i++) {
		if (zeroCount == 2 && src[i] == 0x03) {
			// skip emulation prevention byte
			zeroCount = 0;
			continue;
		}

		m_rbsp.push_back(src[i]);

		if (src[i] == 0x00)
			zeroCount++;
		else
			zeroCount = 0;
	}
}
