// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file audioprocessor.cpp
 * Audio Manipulation Interface
 *
 * @copyright 2009 - 2014 by Johns.  All Rights Reserved.
 * @copyright 2018 by zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "audioprocessor.h"
#include "logger.h"

/******************************************************************************
 * Audio Manipulation
 *****************************************************************************/

/**
 * Create a new audio processoror
 *
 * @param bytesPerSample   bytes per sampler (hardcoded to 2)
 */
cAudioProcessor::cAudioProcessor(const int bytesPerSample)
	: m_bytesPerSample(bytesPerSample)
{
}

/**
 * Normalize audio samples
 *
 * @param samples   sample buffer
 * @param count     number of bytes in sample buffer
 */
void cAudioProcessor::Normalize(uint16_t *samples, int count)
{
	int i;
	int l;
	int n;
	uint32_t avg;
	int factor;
	uint16_t *data;

	// average samples
	l = count / m_bytesPerSample;
	data = samples;
	do {
		n = l;
		if (m_normalizeCounter + n > NORMALIZE_SAMPLES) {
			n = NORMALIZE_SAMPLES - m_normalizeCounter;
		}
		avg = m_normalizeAverage[m_normalizeIndex];
		for (i = 0; i < n; ++i) {
			int t;

			t = data[i];
			avg += (t * t) / NORMALIZE_SAMPLES;
		}
		m_normalizeAverage[m_normalizeIndex] = avg;
		m_normalizeCounter += n;
		if (m_normalizeCounter >= NORMALIZE_SAMPLES) {
			if (m_normalizeReady < NORMALIZE_MAX_INDEX) {
				m_normalizeReady++;
			} else {
				avg = 0;
				for (i = 0; i < NORMALIZE_MAX_INDEX; ++i) {
					avg += m_normalizeAverage[i] / NORMALIZE_MAX_INDEX;
				}

			// calculate normalize factor
			if (avg > 0) {
				factor = ((INT16_MAX / 8) * 1000U) / (uint32_t) sqrt(avg);
				// smooth normalize
				m_normalizeFactor = (m_normalizeFactor * 500 + factor * 500) / 1000;
				if (m_normalizeFactor < NORMALIZE_MIN_FACTOR) {
					m_normalizeFactor = NORMALIZE_MIN_FACTOR;
				}
				if (m_normalizeFactor > m_normalizeMaxFactor) {
					m_normalizeFactor = m_normalizeMaxFactor;
				}
			} else {
				factor = 1000;
			}
			LOGDEBUG2(L_SOUND, "audio: %s: avg %8d, fac=%6.3f, norm=%6.3f", __FUNCTION__,
				avg, factor / 1000.0, m_normalizeFactor / 1000.0);
			}

			m_normalizeIndex = (m_normalizeIndex + 1) % NORMALIZE_MAX_INDEX;
			m_normalizeCounter = 0;
			m_normalizeAverage[m_normalizeIndex] = 0U;
		}
		data += n;
		l -= n;
	} while (l > 0);

	// apply normalize factor
	for (i = 0; i < count / m_bytesPerSample; ++i) {
		int t;

		t = (samples[i] * m_normalizeFactor) / 1000;
		if (t < INT16_MIN) {
			t = INT16_MIN;
		} else if (t > INT16_MAX) {
			t = INT16_MAX;
		}
		samples[i] = t;
	}
}

/**
 * Set normalize volume parameters
 *
 * @param maxfac         max. factor of normalize / 1000
 */
void cAudioProcessor::SetNormalizer(int maxfac)
{
	m_normalizeMaxFactor = maxfac;
}

void cAudioProcessor::ResetNormalizer(void)
{
	m_normalizeCounter = 0;
	m_normalizeReady = 0;

	for (int i = 0; i < NORMALIZE_MAX_INDEX; ++i)
		m_normalizeAverage[i] = 0U;

	m_normalizeFactor = NORMALIZE_DEFAULT_FACTOR;
}

/**
 * Compress audio samples
 *
 * @param samples   sample buffer
 * @param count     number of bytes in sample buffer
 */
void cAudioProcessor::Compress(uint16_t *samples, int count)
{
	int maxSample;
	int i;
	int factor;

	// find loudest sample
	maxSample = 0;
	for (i = 0; i < count / m_bytesPerSample; ++i) {
		int t;

		t = abs(samples[i]);
		if (t > maxSample) {
			maxSample = t;
		}
	}

	// calculate compression factor
	if (maxSample > 0) {
		factor = (INT16_MAX * 1000) / maxSample;
		// smooth compression (FIXME: make configurable?)
		m_compressionFactor = (m_compressionFactor * 950 + factor * 50) / 1000;
		if (m_compressionFactor > factor) {
			m_compressionFactor = factor;	// no clipping
		}
		if (m_compressionFactor > m_compressionMaxFactor) {
			m_compressionFactor = m_compressionMaxFactor;
		}
	} else {
		return; // silent nothing todo
	}

	LOGDEBUG2(L_SOUND, "audio: %s: max %5d, fac=%6.3f, com=%6.3f", __FUNCTION__,
		maxSample, factor / 1000.0, m_compressionFactor / 1000.0);

	// apply compression factor
	for (i = 0; i < count / m_bytesPerSample; ++i) {
		int t;

		t = (samples[i] * m_compressionFactor) / 1000;
		if (t < INT16_MIN) {
			t = INT16_MIN;
		} else if (t > INT16_MAX) {
			t = INT16_MAX;
		}
		samples[i] = t;
	}
}

/**
 * Set volume compression parameters
 *
 * @param maxfac        max. factor of compression / 1000
 */
void cAudioProcessor::SetCompressor(int maxfac)
{
	m_compressionMaxFactor = maxfac;

	if (!m_compressionFactor)
		m_compressionFactor = COMPRESSION_DEFAULT_FACTOR;

	if (m_compressionFactor > m_compressionMaxFactor)
		m_compressionFactor = m_compressionMaxFactor;
}

void cAudioProcessor::ResetCompressor(void)
{
	// reset audio processing values
	m_compressionFactor = COMPRESSION_DEFAULT_MAX_FACTOR;
	if (m_compressionFactor > m_compressionMaxFactor)
		m_compressionFactor = m_compressionMaxFactor;
}

/**
 * Set equalizer bands
 *
 * @param band      setting frequenz bands
 */
void cAudioProcessor::SetEqualizer(int band[18])
{
	int i;
/*
	LOGDEBUG2(L_SOUND, "audio: %s: %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i onoff %d", __FUNCTION__,
	          band[0], band[1], band[2], band[3], band[4], band[5], band[6], band[7],
	          band[8], band[9], band[10], band[11], band[12], band[13], band[14],
	          band[15], band[16], band[17], onoff);
*/
	for (i = 0; i < 18; i++) {
		switch (band[i]) {
			case 1:   m_equalizerBand[i] = 1.5;  break;
			case 0:   m_equalizerBand[i] = 1;    break;
			case -1:  m_equalizerBand[i] = 0.95; break;
			case -2:  m_equalizerBand[i] = 0.9;  break;
			case -3:  m_equalizerBand[i] = 0.85; break;
			case -4:  m_equalizerBand[i] = 0.8;  break;
			case -5:  m_equalizerBand[i] = 0.75; break;
			case -6:  m_equalizerBand[i] = 0.7;  break;
			case -7:  m_equalizerBand[i] = 0.65; break;
			case -8:  m_equalizerBand[i] = 0.6;  break;
			case -9:  m_equalizerBand[i] = 0.55; break;
			case -10: m_equalizerBand[i] = 0.5;  break;
			case -11: m_equalizerBand[i] = 0.45; break;
			case -12: m_equalizerBand[i] = 0.4;  break;
			case -13: m_equalizerBand[i] = 0.35; break;
			case -14: m_equalizerBand[i] = 0.3;  break;
			case -15: m_equalizerBand[i] = 0.25; break;
		}
	}
}

/**
 * Get equalizer filter options
 */
std::string cAudioProcessor::GetEqualizerOptions(void) const
{
	char buffer[256];

	snprintf(buffer, sizeof(buffer),
		"1b=%.2f:2b=%.2f:3b=%.2f"
		":4b=%.2f:5b=%.2f:6b=%.2f"
		":7b=%.2f:8b=%.2f:9b=%.2f"
		":10b=%.2f:11b=%.2f:12b=%.2f"
		":13b=%.2f:14b=%.2f:15b=%.2f:"
		"16b=%.2f:17b=%.2f:18b=%.2f ",
		m_equalizerBand[0],  m_equalizerBand[1],  m_equalizerBand[2],
		m_equalizerBand[3],  m_equalizerBand[4],  m_equalizerBand[5],
		m_equalizerBand[6],  m_equalizerBand[7],  m_equalizerBand[8],
		m_equalizerBand[9],  m_equalizerBand[10], m_equalizerBand[11],
		m_equalizerBand[12], m_equalizerBand[13], m_equalizerBand[14],
		m_equalizerBand[15], m_equalizerBand[16], m_equalizerBand[17]);

	return buffer;
}

/**
 * Amplify the samples in software
 *
 * @param samples   sample buffer
 * @param count     number of bytes in sample buffer
 *
 * @todo FIXME: this does hard clipping
 */
void cAudioProcessor::Amplify(int16_t *samples, int count, int volume)
{
	int i;

	// silence
	if (volume == 0 || !m_amplifier) {
		memset(samples, 0, count);
		return;
	}

	for (i = 0; i < count / m_bytesPerSample; ++i) {
		int t;

		t = (samples[i] * m_amplifier) / 1000;
		if (t < INT16_MIN) {
			t = INT16_MIN;
		} else if (t > INT16_MAX) {
			t = INT16_MAX;
		}
		samples[i] = t;
	}
}
