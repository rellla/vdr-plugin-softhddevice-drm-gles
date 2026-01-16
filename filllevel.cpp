/**
 * @file filllevel.cpp
 * Low-pass filter for audio buffer fill level measurement
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

#include <algorithm>

#include "filllevel.h"
#include "logger.h"

/**
 * Resets the filter state.
 */
void cBufferFillLevelLowPassFilter::Reset()
{
	m_state = UNINITIALIZED;
	m_bufferFillLevelFramesAvg = 0;
	m_frameCounter = 0;
}


/**
 * Resets the received and written frames counters.
 */
void cBufferFillLevelLowPassFilter::ResetFramesCounters()
{
	m_receivedFrames = 0;
	m_writtenToAlsaFrames = 0;
}

/**
 * Updates the buffer fill level average.
 *
 * Calculates the current buffer fill level based on received frames,
 * written frames, and the hardware buffer fill level.
 * Applies a low-pass filter (exponential moving average) to smooth the value.
 * This is necessary, because the packets coming from VDR have a few milliseconds jitter.
 *
 * Handles state transitions:
 * - UNINITIALIZED -> SETTLING: Initializes the average and sets a faster alpha.
 * - SETTLING -> SETTLED: After a duration, switches to a slower alpha and fixes the target value.
 *
 * @param hardwareBufferFillLevelFrames     Current fill level reported by the hardware (ALSA)
 */
void cBufferFillLevelLowPassFilter::UpdateAvgBufferFillLevel(int hardwareBufferFillLevelFrames)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	int bufferFillLevelFrames = m_receivedFrames - m_writtenToAlsaFrames + hardwareBufferFillLevelFrames;

	m_bufferFillLevelFramesAvg = m_bufferFillLevelFramesAvg * (1 - m_floatingAverageAlpha) + bufferFillLevelFrames * m_floatingAverageAlpha;

	switch (m_state) {
		case UNINITIALIZED:
			m_floatingAverageAlpha = FLOATING_AVERAGE_ALPHA_SETTLING;
			m_bufferFillLevelFramesAvg = bufferFillLevelFrames; // init with current value to speed up settling
			m_frameCounter = 0;
			m_state = SETTLING;
			break;
		case SETTLING:
			if (m_frameCounter++ >= SETTLING_DURATION_PACKETS) {
				m_floatingAverageAlpha = FLOATING_AVERAGE_ALPHA_NORMAL;
				// fixate the current buffer fill level as target value, but guarantee a minimum buffer size
				m_bufferFillLevelFramesTargetValue = std::max((double)m_minBufferSizeFrames, m_bufferFillLevelFramesAvg);
				m_state = SETTLED;
			}
			break;
		case SETTLED:
			// nothing
			break;
	}
}

/**
 * Converts the filter state to a string representation.
 *
 * @param state     The state to convert
 * @returns String representation of the state
 */
const char* cBufferFillLevelLowPassFilter::StateToString(State state)
{
	switch(state) {
		case UNINITIALIZED: return "UNINITIALIZED";
		case SETTLING: return "SETTLING";
		case SETTLED: return "SETTLED";
	}
	return "Unknown";
}
