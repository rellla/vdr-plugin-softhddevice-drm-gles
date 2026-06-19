// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file pipreceiver.cpp
 * PiP (Picture-in-Picture) Interface
 *
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <vdr/remux.h>
#include <vdr/skins.h>

#include "logger.h"
#include "pipreceiver.h"
#include "softhddevice.h"

/*****************************************************************************
 * cPipReceiver class
 ****************************************************************************/

/**
 * Create a new receiver for the pip stream
 *
 * Only the video pid is handled
 *
 * @param channel    channel to receive
 * @param device     pointer to cSoftHdDevice object
 */
cPipReceiver::cPipReceiver(const cChannel *channel, cSoftHdDevice *device)
	: cReceiver(NULL, MINPRIORITY),
	  m_pDevice(device)
{
	LOGDEBUG("pipreceiver: %s", __FUNCTION__);
	AddPid(channel->Vpid());
}

/**
 * Detach the pip receiver
 */
cPipReceiver::~cPipReceiver(void)
{
	LOGDEBUG("pipreceiver: %s, try detach", __FUNCTION__);
	Detach();
}

/**
 * Called before the receiver gets attached or after it got detached
 *
 * @param on       set on/off (unused)
 */
void cPipReceiver::Activate(bool on)
{
	LOGDEBUG("pipreceiver: %s %s", __FUNCTION__, on ? "on" : "off");
	m_pTsToPesVideo.Reset();
}

/**
 * Receive data from the receiver
 *
 * This code is taken from VDRs cTransfer::Receive()
 *
 * @note Receive() must return as soon as possible, because it's part of the VDR device main loop
 */
void cPipReceiver::Receive(const uchar *data, int size)
{
	const int MAXRETRIES = 20;    // max. number of retries for a single TS packet
	const int RETRYWAITMS = 5;    // time between two retries
	const int ERRORDELTASEC = 60; // seconds before reporting lost packages again

	for (int i = 0; i < MAXRETRIES; i++) {
		if (ParseTs(data, size) > 0)
			return;
		cCondWait::SleepMs(RETRYWAITMS);
	}
	m_pDevice->ResetPipStream();
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
			int w = m_pDevice->PlayPipVideo(pes, length);
			if (w <= 0) {
				m_pTsToPesVideo.SetRepeatLast();
				return w;
			}
		}
		m_pTsToPesVideo.Reset();
	}
	m_pTsToPesVideo.PutTs(data, size);

	return size;
}

/*****************************************************************************
 * cPipHandler class
 ****************************************************************************/

cPipHandler::cPipHandler(cSoftHdDevice *device)
	: m_pDevice(device)
{
}

cPipHandler::~cPipHandler(void)
{
	Stop();
}

/**
 * Create a new pip receiver and render the pip stream
 *
 * @param channelNum    number of the channel to be switched to
 *                      0 switches to the current main stream channel
 *
 * @retval 0     pip was enabled
 * @retval -1    pip wasn't enabled, no device for channel available
 */
int cPipHandler::Start(int channelNum)
{
	if (!channelNum)
		channelNum = m_pDevice->CurrentChannel();

	const cChannel *channel;
	cDevice *device;
	cPipReceiver *receiver;

	{
		LOCK_CHANNELS_READ;
		channel = Channels->GetByNumber(channelNum);
	}

	device = m_pDevice->GetDevice(channel, 0, false);

	if (channelNum && channel && device) {
		Stop();
		device->SwitchChannel(channel, false);
		receiver = new cPipReceiver(channel, m_pDevice);
		device->AttachReceiver(receiver);
		m_pPipReceiver = receiver;
		m_pPipChannel = channel;
		m_pipChannelNum = channelNum;

		LOGDEBUG("piphandler: %s: New receiver for channel (%d) %s", __FUNCTION__, channel->Number(), channel->Name());

		m_active = true;
		return 0;
	}

	LOGERROR("piphandler: %s: No receiver for channel num %d available", __FUNCTION__, channelNum);
	return -1;
}

/**
 * Delete the pip receiver, clear decoder and display buffers
 * and disable rendering the pip window.
 *
 * We do not need to halt main stream decoder for this,
 * so only halt the PiP decoding and render thread here (in m_pDevice->ResetPipStream())
 */
void cPipHandler::Stop(void)
{
	m_active = false;

	if (!m_pPipReceiver)
		return;

	LOGDEBUG("piphandler: %s: deleting receiver for channel (%d) %s", __FUNCTION__, m_pPipChannel->Number(), m_pPipChannel->Name());

	// both, PiP decoding and render thread are halted and resumed in ResetPipStream
	m_pDevice->ResetPipStream();

	delete m_pPipReceiver;
	m_pPipReceiver = nullptr;
	m_pPipChannel = nullptr;
}

/**
 * Enable/ disable picture-in-picture
 *
 * @param on       true, if pip should be enabled
 */
void cPipHandler::HandleEnable(bool on)
{
	if (on && m_active) {
		LOGDEBUG("piphandler: %s: pip is already enabled", __FUNCTION__);
	} else if (on && !m_active) {
		LOGDEBUG("piphandler: %s: enabling pip (channel %d)", __FUNCTION__, m_pipChannelNum);
		if (!Start(0))
			m_pDevice->SetRenderPipActive(true);
	} else if (!on && !m_active) {
		LOGDEBUG("piphandler: %s: pip is already disabled", __FUNCTION__);
	} else if (!on && m_active){
		LOGDEBUG("piphandler: %s: disabling pip", __FUNCTION__);
		Stop();
		m_pDevice->SetRenderPipActive(false);
	}
}

/**
 * Change the pip channel
 *
 * @param direction      1: channel up, -1: channel down
 */
void cPipHandler::HandleChannelChange(int direction)
{
	if (!m_active)
		return;

	const cChannel *channel = m_pPipChannel;
	const cChannel *first = m_pPipChannel;

	Stop();

	while (channel) {
		bool ndr;

		{
			LOCK_CHANNELS_READ;
			channel = direction > 0 ? Channels->Next(channel) : Channels->Prev(channel);
			if (!channel && Setup.ChannelsWrap)
				channel = direction > 0 ? Channels->First() : Channels->Last();
		}

		cDevice *device = m_pDevice->GetDevice(channel, 0, false, true);

		if (channel && !channel->GroupSep() && device && device->ProvidesChannel(channel, 0, &ndr) && !ndr) {
			Start(channel->Number());
			return;
		}

		if (channel == first) {
			Skins.Message(mtError, tr("Channel not available!"));
			break;
		}
	}
}

/*****************************************************************************
 * PiP handler public API
 *
 * These (public) functions are wrapped by cSoftHdDevice.
 ****************************************************************************/

/**
 * Start picture-in-picture
 */
void cPipHandler::Enable(void)
{
	if (m_active)
		return;

	HandleEnable(true);
}

/**
 * Stop picture-in-picture
 */
void cPipHandler::Disable(void)
{
	if (!m_active)
		return;

	HandleEnable(false);
}

/**
 * Toggle picture-in-picture
 */
void cPipHandler::Toggle(void)
{
	HandleEnable(!m_active);
}

/**
 * Change the pip channel
 *
 * @param direction      1: channel up, -1: channel down
 */
void cPipHandler::ChannelChange(int direction)
{
	if (!m_active)
		return;

	if (direction > 0)
		HandleChannelChange(1);
	else
		HandleChannelChange(-1);
}

/**
 * Swap the pip channel with main live channel
 *
 * @param closePip      close the pip window after the channel swap
 */
void cPipHandler::ChannelSwap(bool closePip)
{
	if (!m_active)
		return;

	const cChannel *channel = m_pPipChannel;
	if (!channel)
		return;

	Stop();
	if (!closePip)
		Start(0); // resets the pip channel to the current channel

	LOGDEBUG("piphandler: %s: switch main stream to %d", __FUNCTION__, channel->Number());
	{
		LOCK_CHANNELS_READ;
		Channels->SwitchTo(channel->Number());
	}
}

/**
 * Set size and position for the pip window
 */
void cPipHandler::SetSize(void)
{
	m_pDevice->SetRenderPipSize();
}

/**
 * Swap pip between normal and alternative position
 */
void cPipHandler::SwapPosition(void)
{
	m_pDevice->ToggleRenderPipPosition();
	m_pDevice->SetRenderPipSize();
}
