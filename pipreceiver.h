// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file pipreceiver.h
 * PiP (Picture-in-Picture) Interface Header File
 *
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __PIPRECEIVER_H
#define __PIPRECEIVER_H

#include <vdr/receiver.h>

#include "event.h"

class cSoftHdDevice;

/**
 * Picture-in-Picture
 * @defgroup pip PiP Handler
 */

/**
 * Receiver for PiP Stream
 *
 * @ingroup pip
 */
class cPipReceiver : public cReceiver {
public:
	cPipReceiver(const cChannel *, cSoftHdDevice *);
	virtual ~cPipReceiver(void);

protected:
	virtual void Activate(bool);
	virtual void Receive(const uchar *, int);

private:
	cSoftHdDevice *m_pDevice;        ///< pointer to device
	cTsToPes m_pTsToPesVideo;        ///< TS to PES converter
	uint64_t m_lastErrorReport = 0;  ///< tracks time since last error report
	int m_numLostPackets = 0;        ///< tracks lost packets

	int ParseTs(const uchar *, int);
	int PlayTs(const uchar *, int);
};

/**
 * PiP Stream Handler
 *
 * @ingroup pip
 */
class cPipHandler {
public:
	cPipHandler(cSoftHdDevice *);
	virtual ~cPipHandler(void);

	bool IsEnabled(void) { return m_active; };
	void Enable(void);
	void Disable(void);
	void Toggle(void);
	void ChannelChange(int);
	void ChannelSwap(bool);
	void SetSize(void);
	void SwapPosition(void);
	void HandleEvent(enum PipState);

private:
	cSoftHdDevice *m_pDevice;               ///< pointer to device
	IEventReceiver *m_pEventReceiver;       ///< pointer to event receiver
	cPipReceiver *m_pPipReceiver = nullptr; ///< pointer to pip receiver
	int m_pipChannelNum = 0;                ///< current pip channel number
	const cChannel *m_pPipChannel;          ///< current pip channel
	bool m_active = false;                  ///< true, if pip is active

	int Start(int);
	void Stop(void);
	void HandleEnable(bool);
	void HandleChannelChange(int);
};

#endif
