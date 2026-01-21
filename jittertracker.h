/**
 * @file jittertracker.h
 * Tracks the jitter of incoming packets (currently only for logging purposes)
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

#ifndef JITTERTRACKER_H
#define JITTERTRACKER_H

#include <chrono>

class cJitterTracker {
public:
	cJitterTracker(const char* identifier) : m_identifier(identifier) {}
	void PacketReceived();
	void Reset();

private:
	int64_t m_lastDiffMs = 0;
	std::chrono::steady_clock::time_point m_lastTime;
	int m_shortTermMaxJitterMs = 0;
	int m_packetCounter = 0;
	int m_longTermMaxJitterMs = 0;
	bool m_firstPacket = true;
	bool m_secondPacket = true;
	const char *m_identifier;
};

#endif
