/**
 * @file fillLevel.h
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

#ifndef FILLLEVEL_H
#define FILLLEVEL_H

#include <mutex>

class cBufferFillLevelLowPassFilter {
public:
	void Reset();
	void ResetFramesCounters();
	int GetFramesReceived() { std::lock_guard<std::mutex> lock(m_mutex); return m_receivedFrames; }
	int GetFramesPlayed() { std::lock_guard<std::mutex> lock(m_mutex); return m_writtenToAlsaFrames; }
	void ReceivedFrames(int count) { std::lock_guard<std::mutex> lock(m_mutex); m_receivedFrames += count; }
	void WroteFrames(int count) { std::lock_guard<std::mutex> lock(m_mutex); m_writtenToAlsaFrames += count; }
	double GetBufferFillLevelFramesAvg() { std::lock_guard<std::mutex> lock(m_mutex); return m_bufferFillLevelFramesAvg; }
	void SetMinBufferSizeFrames(int size) { std::lock_guard<std::mutex> lock(m_mutex); m_minBufferSizeFrames = size; }
	void UpdateAvgBufferFillLevel(int);
	bool IsSettled() { std::lock_guard<std::mutex> lock(m_mutex); return m_state == SETTLED; }

private:
	enum State {
		UNINITIALIZED,
		SETTLING,
		SETTLED,
	};

	constexpr static double FLOATING_AVERAGE_ALPHA_SETTLING = 0.02;
	constexpr static double FLOATING_AVERAGE_ALPHA_NORMAL = 0.002;
	constexpr static int SETTLING_DURATION_PACKETS = 30 / 0.16; // 30 seconds with one packet every 160ms

	std::mutex m_mutex;
	State m_state = UNINITIALIZED;
	int m_receivedFrames = 0;
	int m_writtenToAlsaFrames = 0;
	double m_bufferFillLevelFramesAvg = 0;
	double m_bufferFillLevelFramesTargetValue = 0;
	int m_frameCounter = 0;
	double m_floatingAverageAlpha = FLOATING_AVERAGE_ALPHA_NORMAL;
	int m_minBufferSizeFrames = 0;

	const char* StateToString(State d);
};

#endif
