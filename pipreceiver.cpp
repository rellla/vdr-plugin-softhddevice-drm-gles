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
 * called before the the receiver gets attached or detached
 */
void cPipReceiver::Activate(bool on)
{
	LOGDEBUG("pipreceiver: %s %s", __FUNCTION__, on ? "on" : "off");
	if (on) {
// TODO		m_pDevice->PipStart();
	} else {
// TODO		m_pDevice->PipStop();
	}
}

void cPipReceiver::Receive(const uchar *data, int size)
{
	(void)data;
	(void)size;
// TS parser
// PES parser
// PlayVideo
}