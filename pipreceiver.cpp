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
#include <vdr/skins.h>

#include "event.h"
#include "logger.h"
#include "pipreceiver.h"
#include "softhddevice.h"

/*****************************************************************************
 * cPipReceiver class
 ****************************************************************************/

/**
 * pip receiver class constructor
 */
cPipReceiver::cPipReceiver(const cChannel *channel, cSoftHdDevice *device)
	: cReceiver(NULL, MINPRIORITY),
	  m_pDevice(device)
{
	LOGDEBUG("pipreceiver: %s", __FUNCTION__);
	AddPid(channel->Vpid());
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

/**
 * cPipHandler constructor
 */
cPipHandler::cPipHandler(cSoftHdDevice *device)
	: m_pDevice(device),
	  m_pEventReceiver(device)
{
}

cPipHandler::~cPipHandler(void)
{
	Stop();
}

/*****************************************************************************
 * Handle events
 *
 * The following functions are called from within the state change and must
 * not trigger any new events. Otherwise we end up in a dead lock!
 ****************************************************************************/

/**
 * Handle the pip event
 */
void cPipHandler::HandleEvent(enum PipState event)
{
	switch (event) {
		case PIPSTART:
			HandleEnable(true);
			break;
		case PIPSTOP:
			HandleEnable(false);
			break;
		case PIPTOGGLE:
			HandleEnable(!m_active);
			break;
		case PIPCHANUP:
			HandleChannelChange(1);
			break;
		case PIPCHANDOWN:
			HandleChannelChange(-1);
			break;
		case PIPCHANSWAP:
			Stop();
			Start(0);
			break;
		case PIPSIZECHANGE:
			m_pDevice->SetRenderPipSize();
			break;
		case PIPSWAPPOSITION:
			m_pDevice->ToggleRenderPipPosition();
			m_pDevice->SetRenderPipSize();
			break;
		default:
			break;
	}
}

/**
 * Create a new pip receiver and render the pip stream
 *
 * @param channelNum    number of the channel to be switched to
 *                      0 switches to the current main stream channel
 *
 * @retval 0     pip was enabled
 * @retval -1    pip wasn't enabled, no device for channel available
 *
 * @note This function is called within the state change and must not trigger any new events!
 */
int cPipHandler::Start(int channelNum)
{
	if (!channelNum)
		channelNum = m_pDevice->CurrentChannel();

	LOCK_CHANNELS_READ;
	const cChannel *channel;
	cDevice *device;
	cPipReceiver *receiver;

	if (channelNum && (channel = Channels->GetByNumber(channelNum)) &&
	   (device = m_pDevice->GetDevice(channel, 0, false, false))) {
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
 * We do not need to halt main stream decoder and display thread for this,
 * so only halt the pip decoding thread here (in m_pDevice->ResetPipStream()) - not in OnEventReceived().
 *
 * @note This function is called within the state change and must not trigger any new events!
 */
void cPipHandler::Stop(void)
{
	m_active = false;

	if (!m_pPipReceiver)
		return;

	LOGDEBUG("piphandler: %s: deleting receiver for channel (%d) %s", __FUNCTION__, m_pPipChannel->Number(), m_pPipChannel->Name());

	m_pDevice->ResetPipStream();

	delete m_pPipReceiver;
	m_pPipReceiver = nullptr;
	m_pPipChannel = nullptr;
}

/**
 * Enable/ disable picture-in-picture
 *
 * @param on       true, if pip should be enabled
 *
 * @note This function is called within the state change and must not trigger any new events!
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
		m_pDevice->SetRenderPipActive(false);
		Stop();
	}
}

/**
 * Change the pip channel
 *
 * @param direction      1: channel up, -1: channel down
 *
 * @note This function is called within the state change and must not trigger any new events!
 */
void cPipHandler::HandleChannelChange(int direction)
{
	if (!m_active)
		return;

	const cChannel *channel;
	const cChannel *first;

	channel = m_pPipChannel;
	first = channel;

	Stop();

	LOCK_CHANNELS_READ;
	while (channel) {
		bool ndr;
		cDevice *device;

		channel = direction > 0 ? Channels->Next(channel) : Channels->Prev(channel);
		if (!channel && Setup.ChannelsWrap)
			channel = direction > 0 ? Channels->First() : Channels->Last();

		if (channel && !channel->GroupSep() && (device = cDevice::GetDevice(channel, 0, false, true)) &&
			device->ProvidesChannel(channel, 0, &ndr) && !ndr) {
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
 * Trigger events
 *
 * These (public) functions are wrapped by cSoftHdDevice and can be called
 * to trigger a pip event.
 ****************************************************************************/

/**
 * Start picture-in-picture
 */
void cPipHandler::Enable(void)
{
	if (m_active)
		return;

	m_pEventReceiver->OnEventReceived(PipEvent{PIPSTART});
}

/**
 * Stop picture-in-picture
 */
void cPipHandler::Disable(void)
{
	if (!m_active)
		return;

	m_pEventReceiver->OnEventReceived(PipEvent{PIPSTOP});
}

/**
 * Toggle picture-in-picture
 */
void cPipHandler::Toggle(void)
{
	m_pEventReceiver->OnEventReceived(PipEvent{PIPTOGGLE});
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
		m_pEventReceiver->OnEventReceived(PipEvent{PIPCHANUP});
	else
		m_pEventReceiver->OnEventReceived(PipEvent{PIPCHANDOWN});
}

/**
 * Swap the pip channel with main live channel
 *
 * The channel switch of the main stream must be done out of OnEventReceived()
 * because it triggers a SetPlayMode() which end in a deadlock otherwise.
 */
void cPipHandler::ChannelSwap(void)
{
	if (!m_active)
		return;

	const cChannel *channel = m_pPipChannel;
	if (!channel)
		return;

	m_pEventReceiver->OnEventReceived(PipEvent{PIPCHANSWAP}); // resets the pip channel to the current channel

	LOCK_CHANNELS_READ;
	LOGDEBUG("piphandler: %s: switch main stream to %d", __FUNCTION__, channel->Number());
	Channels->SwitchTo(channel->Number());
}

/**
 * Set size and position for the pip window
 */
void cPipHandler::SetSize(void)
{
	m_pEventReceiver->OnEventReceived(PipEvent{PIPSIZECHANGE});
}

/**
 * Swap pip between normal and alternative position
 */
void cPipHandler::SwapPosition(void)
{
	m_pEventReceiver->OnEventReceived(PipEvent{PIPSWAPPOSITION});
}
