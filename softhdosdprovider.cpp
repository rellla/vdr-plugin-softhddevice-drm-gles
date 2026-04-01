// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file softhdosdprovider.cpp
 * OSD Provider
 *
 * This file provides cSoftOsd which is the software accelerated
 * version of this plugin (in contrast to the hardware accelerater cOglOsd).
 * It also decribes cSoftOsdProvider.
 *
 * @copyright 2011, 2015 by Johns.  All Rights Reserved.
 * @copyright 2018 zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include "dummyosd.h"
#include "logger.h"
#ifdef USE_GLES
#include "openglosd.h"
#endif
#include "softhddevice.h"
#include "softhdosd.h"
#include "softhdosdprovider.h"

/**
 * OSD Provider
 *
 * @addtogroup osd
 * @{
 */

/*****************************************************************************
 * OSD provider
 ****************************************************************************/

/**
 * Create a new OSD provider
 */
cSoftOsdProvider::cSoftOsdProvider(cSoftHdDevice *device)
	: cOsdProvider(),
	  m_pDevice(device)
{
	LOGDEBUG2(L_OSD, "osdprovider: %s:", __FUNCTION__);
}

/**
 * Delete the OSD provider and stop the OpenGL thread if running
 */
cSoftOsdProvider::~cSoftOsdProvider()
{
	LOGDEBUG2(L_OSD, "osdprovider %s:", __FUNCTION__);

	if (m_pDevice->IsOsdProviderSet()) {
		m_pDevice->ResetOsdProvider();
#ifdef USE_GLES
		if (!m_pDevice->OglOsdIsDisabled())
			StopOpenGlThread();
#endif
	}
}

/**
 * Create a new OSD
 *
 * Create either a hardware accelerated (cOglOsd), software based (cSoftOsd) or dummy OSD (if detached)
 *
 * @param left   x-coordinate of OSD
 * @param top    y-coordinate of OSD
 * @param level  layer level of OSD
 */
cOsd *cSoftOsdProvider::CreateOsd(int left, int top, uint level)
{
#ifdef USE_GLES
	if (m_pDevice->IsDetached()) {
		LOGDEBUG("osdprovider: %s: %d, %d, %d, device detached, using dummy osd", __FUNCTION__, left, top, level);
		return m_pOsd = new cDummyOsd(left, top, level);
	}

	if (m_pDevice->OglOsdIsDisabled()) {
		LOGDEBUG("osdprovider: %s: %d, %d, %d, OpenGL disabled, using software rendering", __FUNCTION__, left, top, level);
		return m_pOsd = new cSoftOsd(left, top, level, m_pDevice);
	}

	if (StartOpenGlThread()) {
		LOGDEBUG2(L_OSD, "osdprovider: %s: %d, %d, %d, using OpenGL OSD support", __FUNCTION__, left, top, level);
		return m_pOsd = new cOglOsd(left, top, level, m_pOglThread, m_pDevice);
	}

	LOGDEBUG("osdprovider: %s: %d, %d, %d, OpenGL failed, using software rendering", __FUNCTION__, left, top, 999);
	m_pDevice->SetDisableOglOsd();
	return m_pOsd = new cSoftOsd(left, top, 999, m_pDevice);
#else
	if (m_pDevice->IsDetached()) {
		LOGDEBUG("osdprovider: %s: %d, %d, %d, device detached, using dummy osd", __FUNCTION__, left, top, level);
		return m_pOsd = new cDummyOsd(left, top, level);
	}

	LOGDEBUG2(L_OSD, "osdprovider: %s: %d, %d, %d", __FUNCTION__, left, top, level);
	return m_pOsd = new cSoftOsd(left, top, level, m_pDevice);
#endif
}

#ifdef USE_GLES
/**
 * Stop the OpenGL thread, if the osd size changed and update the size
 */
void cSoftOsdProvider::OsdSizeChanged(void) {
	// cleanup OpenGL context
	if (!m_pDevice->OglOsdIsDisabled())
		cSoftOsdProvider::StopOpenGlThread();
	cSoftOsdProvider::UpdateOsdSize();
}

/**
 * Start the OpenGL thread
 */
bool cSoftOsdProvider::StartOpenGlThread(void) {
	if (m_pDevice->OglOsdIsDisabled()) {
		LOGDEBUG2(L_OPENGL, "osdprovider: %s: OpenGL OSD disabled, OpenGL worker thread NOT started", __FUNCTION__);
		return false;
	}

	if (m_pOglThread.get()) {
		if (m_pOglThread->Active()) {
			return true;
		}
		m_pOglThread.reset();
	}
	cCondWait wait;
	LOGDEBUG2(L_OPENGL, "osdprovider: %s: Trying to start OpenGL worker thread", __FUNCTION__);
	m_pOglThread.reset(new cOglThread(&wait, m_pDevice->MaxSizeGPUImageCache(), m_pDevice));
	wait.Wait();

	if (m_pOglThread->Active()) {
		LOGINFO("OpenGL worker thread started");
		return true;
	}

	LOGDEBUG2(L_OPENGL, "osdprovider: %s: OpenGL worker thread NOT started", __FUNCTION__);
	return false;
}

/**
 * Initiate a stop of the OpenGL thread without waiting
 */
void cSoftOsdProvider::RequestStopOpenGlThread(void) {
	if (m_pOglThread) {
		LOGDEBUG2(L_OPENGL, "osdprovider: %s: request stopping OpenGL worker thread", __FUNCTION__);
		m_pOglThread->RequestStop();
	}
}

/**
 * Stop the OpenGL thread and cancel it if necessary
 */
void cSoftOsdProvider::StopOpenGlThread(void) {
	if (m_pOglThread) {
		LOGDEBUG2(L_OPENGL, "osdprovider: %s: stop OpenGL worker thread", __FUNCTION__);
		m_pOglThread->Stop();
	}
	m_pOglThread.reset();
}

/**
 * Lock the OpenGL thread
 */
bool cSoftOsdProvider::LockOpenGlThread(void) {
	if (m_pOglThread) {
		LOGDEBUG2(L_OPENGL, "osdprovider: %s: lock OpenGL worker thread", __FUNCTION__);
		m_pOglThread->LockOutputFb();
		return true;
	}

	return false;
}

/**
 * Unlock the OpenGL thread
 */
void cSoftOsdProvider::UnlockOpenGlThread(void) {
	if (m_pOglThread) {
		LOGDEBUG2(L_OPENGL, "osdprovider: %s: unlock OpenGL worker thread", __FUNCTION__);
		m_pOglThread->UnlockOutputFb();
	}
}

/**
 * Store image data
 */
int cSoftOsdProvider::StoreImageData(const cImage &Image)
{
	if (StartOpenGlThread()) {
		int imgHandle = m_pOglThread->StoreImage(Image);
		return imgHandle;
	}
	return 0;
}

/**
 * Get stored image data
 */
const cImage *cSoftOsdProvider::GetImageData(int ImageHandle) {
	return cOsdProvider::GetImageData(ImageHandle);
}

/**
 * Drop stored image data
 */
void cSoftOsdProvider::DropImageData(int imgHandle)
{
	if (StartOpenGlThread())
		m_pOglThread->DropImageData(imgHandle);
}
#endif

/** @} */
