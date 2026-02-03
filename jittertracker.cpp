// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file jittertracker.cpp
 * Jitter Tracking of Incoming Packets
 *
 * @license{AGPL-3.0-or-later}
 */

#include <algorithm>
#include <cmath>

#include "jittertracker.h"
#include "logger.h"

/**
 * @addtogroup misc
 * @{
 */

/**
 * Called each time a packet is received.
 *
 * Calculates the diff in PTS to the last received packet and compares that
 * to the diff of the wall clock time of the last received packet to now.
 */
void cJitterTracker::PacketReceived(void)
{
	auto now = std::chrono::steady_clock::now();

	m_packetCounter++;

	if (m_firstPacket) {
		m_lastTime = now;
		m_firstPacket = false;

		return;
	}

	auto diffMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTime).count();

	if (m_secondPacket) {
		m_lastDiffMs = diffMs;
		m_lastTime = now;
		m_secondPacket = false;

		return;
	}

	int jitterMs = std::abs(m_lastDiffMs - diffMs);

	if (jitterMs > m_longTermMaxJitterMs)
		m_longTermMaxJitterMs = jitterMs;

	if (jitterMs > m_shortTermMaxJitterMs)
		m_shortTermMaxJitterMs = jitterMs;

	// if (jitterMs > 35 && strcmp(m_identifier, "video") == 0)
	//     LOGDEBUG2(L_SOUND, "jittertracker: %s: %s: high jitter detected: packet no.: %d: %dms (last diff: %dms, time diff: %dms)", __FUNCTION__, m_identifier, m_packetCounter, jitterMs, m_lastDiffMs, diffMs);

	if (m_packetCounter % 1000 == 0) {
		// LOGDEBUG2(L_SOUND, "jittertracker: %s: %s: max jitter: last 1000 packets: %dms, overall: %dms,", __FUNCTION__, m_identifier, m_shortTermMaxJitterMs, m_longTermMaxJitterMs);
		m_shortTermMaxJitterMs = 0;
	}

	m_lastDiffMs = diffMs;
	m_lastTime = now;
}

/**
 * Resets the jitter tracker.
 */
void cJitterTracker::Reset(void)
{
	m_lastDiffMs = 0;
	m_shortTermMaxJitterMs = 0;
	m_packetCounter = 0;
	m_longTermMaxJitterMs = 0;
	m_firstPacket = true;
	m_secondPacket = true;
}

/** @} */
