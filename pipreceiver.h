/**
 * @file pipreceiver.h
 * Pip receiver header file
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

#ifndef __PIPRECEIVER_H
#define __PIPRECEIVER_H

#include <vdr/receiver.h>

#include "softhddevice.h"

/**
 * cPipReceiver - receiver class for pip
 */
class cPipReceiver : public cReceiver
{
public:
	cPipReceiver(const cChannel *, cSoftHdDevice *);
	virtual ~cPipReceiver(void);

protected:
	virtual void Activate(bool);
	virtual void Receive(const uchar *, int);

private:
	cSoftHdDevice *m_pDevice;
	cTsToPes m_pTsToPesVideo;
	uint64_t m_lastErrorReport = 0;
	int m_numLostPackets = 0;

	int ParseTs(const uchar *, int);
	int PlayTs(const uchar *, int);
};

#endif
