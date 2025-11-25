/**
 * @file pipreceiver.cpp
 * pip receiver class
 *
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

#include <vdr/remux.h>

#include "logger.h"
#include "pipreceiver.h"
#include "softhddevice.h"

/*****************************************************************************
 * cPipReceiver class
 ****************************************************************************/

/**
 * pip receiver class constructor
 */
cPipReceiver::cPipReceiver(const cChannel *channel, cSoftHdDevice *device) : cReceiver(NULL, MINPRIORITY)
{
	LOGDEBUG("pipreceiver: %s", __FUNCTION__);
	m_pDevice = device;
	AddPid(channel->Vpid());
	m_lastErrorReport = 0;
	m_numLostPackets = 0;
}

/**
 * pip receiver class destructor
 */
cPipReceiver::~cPipReceiver(void)
{
	LOGDEBUG("pipreceiver: %s", __FUNCTION__);
	Detach();
}

/**
 * called before the receiver gets attached or after it got detached
 */
void cPipReceiver::Activate(bool on)
{
	LOGDEBUG("pipreceiver: %s %s", __FUNCTION__, on ? "on" : "off");
	m_pTsToPesVideo.Reset();
}

#define MAXRETRIES    20 // max. number of retries for a single TS packet
#define RETRYWAITMS    5 // time between two retries
#define ERRORDELTASEC 60 // seconds before reporting lost packages again
/**
 * receive data from the receiver
 *
 * This code is taken from VDRs cTransfer::Receive()
 */
void cPipReceiver::Receive(const uchar *data, int size)
{
	for (int i = 0; i < MAXRETRIES; i++) {
		if (ParseTs(data, size) > 0)
			return;
		cCondWait::SleepMs(RETRYWAITMS);
	}
	m_numLostPackets++;
	if (cTimeMs::Now() - m_lastErrorReport > ERRORDELTASEC) {
		LOGWARNING("pipreceiver: %d TS packet(s) not accepted in pip stream", m_numLostPackets);
		m_numLostPackets = 0;
		m_lastErrorReport = cTimeMs::Now();
	}
}

/**
 * Parse the ts stream and send it to the pes player
 *
 * This code is taken from VDRs cDevice::PlayTs()
 */
int cPipReceiver::ParseTs(const uchar *data, int size)
{
	int played = 0;

	if (!data) {
		LOGWARNING("pipreceiver: %s null data received, reset pes buffer!", __FUNCTION__);
		m_pTsToPesVideo.Reset();
		return 0;
	}

	if (size < TS_SIZE) {
		LOGWARNING("pipreceiver: %s TS fragment received!", __FUNCTION__);
		return size;
	}

	while (size >= TS_SIZE) {
		if (int skipped = TS_SYNC(data, size)) {
			LOGWARNING("pipreceiver: %s TS stream not in sync!", __FUNCTION__);
			return played + skipped;
		}

		if (TsHasPayload(data)) {
			int payloadOffset = TsPayloadOffset(data);
			if (payloadOffset < TS_SIZE) {
				int w = PlayTs(data, TS_SIZE);
				if (w < 0)
					return played ? played : w;
				if (w == 0)
					break;
			}
		}

		played += TS_SIZE;
		size -= TS_SIZE;
		data += TS_SIZE;
	}

	return played;
}

/**
 * Get the pes payload and send it to the player
 *
 * This code is taken from VDRs cDevice::PlayTsVideo()
 */
int cPipReceiver::PlayTs(const uchar *data, int size)
{
	if (TsPayloadStart(data)) {
		int length;
		while (const uchar *pes = m_pTsToPesVideo.GetPes(length)) {
			LOGDEBUG("pipreceiver: %s %p %d", __FUNCTION__, pes, length);
/*
			int w = m_pDevice->PlayPipVideo(pes, length);
			if (w <= 0) {
				m_pTsToPesVideo.SetRepeatLast();
				return w;
			}
*/
		}
		m_pTsToPesVideo.Reset();
	}
	m_pTsToPesVideo.PutTs(data, size);

	return size;
}
