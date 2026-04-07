// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file jittertracker.h
 * Jitter Tracking of Incoming Packets Header File
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef JITTERTRACKER_H
#define JITTERTRACKER_H

#include <atomic>
#include <chrono>

/**
 * Jitter Tracker
 *
 * @ingroup misc
 */
class cJitterTracker {
public:
	cJitterTracker(const char* identifier) : m_identifier(identifier) {}
	void PacketReceived(void);
	void Reset(void);
	int GetLongTermMaxJitterMs(void) { return m_longTermMaxJitterMs; };
	int GetShortTermMaxJitterMs(void) { return m_shortTermMaxJitterMs; };

private:
	int64_t m_lastDiffMs = 0;
	std::chrono::steady_clock::time_point m_lastTime;
	std::atomic<int> m_shortTermMaxJitterMs = 0;
	int m_packetCounter = 0;
	std::atomic<int> m_longTermMaxJitterMs = 0;
	bool m_firstPacket = true;
	bool m_secondPacket = true;
	const char *m_identifier;
};

#endif
