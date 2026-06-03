// SPDX-License-Identifier: AGLP-3.0-or-later

/**
 * @file audioprocessor.h
 * Audio Manipulation Interface Header File
 *
 * @copyright 2009 - 2014 by Johns.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __AUDIOPROCESSOR_H
#define __AUDIOPROCESSOR_H

#include <cstdint>
#include <string>

/**
 * Audio Manipulation
 *
 * @ingroup audio
 */
class cAudioProcessor {
public:
	cAudioProcessor(const int);

	void Normalize(uint16_t *, int);
	void SetNormalizer(int);
	void ResetNormalizer(void);

	void Compress(uint16_t *, int);
	void SetCompressor(int);
	void ResetCompressor(void);

	void SetEqualizer(int[18]);
	std::string GetEqualizerOptions(void) const;

	void Amplify(int16_t *, int, int);
	void SetAmplifier(int volume) { m_amplifier = volume; };

private:
	const int m_bytesPerSample;

	// Normalizer
	constexpr static int NORMALIZE_MAX_INDEX = 128;       ///< number of normalize average samples
	constexpr static int NORMALIZE_SAMPLES = 4096;        ///< number of normalize samples
	constexpr static int NORMALIZE_DEFAULT_FACTOR = 1000; ///< default normalize factor
	constexpr static int NORMALIZE_MIN_FACTOR = 100;      ///< min. normalize factor
	int m_normalizeCounter;                 ///< normalize sample counter
	uint32_t m_normalizeAverage[NORMALIZE_MAX_INDEX]; ///< average of n last normalize sample blocks
	int m_normalizeIndex;                   ///< index into normalize average table
	int m_normalizeReady;                   ///< index normalize counter
	int m_normalizeFactor;                  ///< current normalize factor
	int m_normalizeMaxFactor;               ///< max. normalize factor

	// Compressor
	constexpr static int COMPRESSION_DEFAULT_FACTOR = 1000;     ///< default compression factor
	constexpr static int COMPRESSION_DEFAULT_MAX_FACTOR = 2000; ///< default compression max. factor
	int m_compressionFactor = 0;            ///< current compression factor
	int m_compressionMaxFactor;             ///< max. compression factor

	// Equalizer
	float m_equalizerBand[18];              ///< equalizer band

	// Amplifier
	int m_amplifier;                        ///< software volume amplify factor
};

#endif
