// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file softhddevice.cpp
 * Output Device
 *
 * This file defines cSoftHdDevice which is the implementation
 * of cDevice. This is the place where all the device commands
 * which are sent be VDR are placed in (i.e. Play(), TrickSpeed() ...)
 *
 * @copyright 2011 - 2015 by Johns.  All Rights Reserved.
 * @copyright 2018 - 2019 by zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <chrono>
#include <mutex>
#include <variant>
#include <libintl.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <vdr/dvbspu.h>
#include <vdr/skins.h>
#include <vdr/status.h>
#include <vdr/thread.h>

#include "audio.h"
#include "codec_audio.h"
#include "config.h"
#include "grab.h"
#include "hardwaredevice.h"
#include "jittertracker.h"
#include "logger.h"
#include "pes.h"
#include "pipreceiver.h"
#include "softhddevice.h"
#include "softhdosdprovider.h"
#include "statemachine.h"
#include "videorender.h"
#include "videostream.h"

/**
 * Create the device
 *
 * Initializes some member variables
 *
 * @param config       pointer to cSoftHdConfig class
 */
cSoftHdDevice::cSoftHdDevice(cSoftHdConfig *config)
	: m_pConfig(config)
{
	m_pStateMachine = std::make_unique<cStateMachine>(this);
}

/**
 * Destroy the device
 *
 * Only delete objects, if they were created in Initialize()
 */
cSoftHdDevice::~cSoftHdDevice(void)
{
	if (!m_initialized)
		return;

	m_initialized = false; // not necessary, just for documentation

	delete m_pEventHandler;
	delete m_pHardwareDevice;
	delete m_pSpuDecoder;
}

/*********************************************************************
 * VDR cPlugin interface (wrapped by cPluginSoftHdDevice)
 ********************************************************************/

/**
 * Initialize the device
 */
bool cSoftHdDevice::Initialize(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);

	// the following are deleted in the destructor
	m_pSpuDecoder = new cDvbSpuDecoder();
	m_pHardwareDevice = new cHardwareDevice();
	m_pEventHandler = new cEventHandler(m_pStateMachine.get());

	m_channelSwitchStartTime = std::chrono::steady_clock::now();
	m_dataReceivedTime = m_channelSwitchStartTime;

	m_pipUseAlt = m_pConfig->ConfigPipUseAlt;

	m_initialized = true;

	return true;
}

/**
 * Called by VDR when the plugin is started.
 */
int cSoftHdDevice::Start(void)
{
	LOGDEBUG("device: %s", __FUNCTION__);
	TriggerEvent(AttachEvent{});

	return true;
}

/**
 * Called by VDR when the plugin is stopped.
 */
void cSoftHdDevice::Stop(void)
{
	LOGDEBUG("device: %s", __FUNCTION__);
	m_pPipHandler->Disable();
	TriggerEvent(DetachEvent{});
}

/*********************************************************************
 * VDR cDevice interface
 ********************************************************************/

/**
 * Informs a device that it will be the primary device
 *
 * @param on	flag if becoming or loosing primary
 */
void cSoftHdDevice::MakePrimaryDevice(bool on)
{
	LOGDEBUG("device: %s: %d", __FUNCTION__, on);

	if (on)
		m_pOsdProvider = new cSoftOsdProvider(this); // no need to delete it, VDR does it

	cDevice::MakePrimaryDevice(on);
}

/**
 * Tells whether this device has an MPEG decoder
 */
bool cSoftHdDevice::HasDecoder(void) const
{
	bool hasDecoder = !IsDetached();

//	LOGDEBUG("device: %s: %d", __FUNCTION__, hasDecoder);

	return hasDecoder;
}

/**
 * Get the device SPU decoder.
 *
 * @return a pointer to the device's SPU decoder
 *         (or NULL, if this device doesn't have an SPU decoder)
 */
cSpuDecoder *cSoftHdDevice::GetSpuDecoder(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	if (!IsPrimaryDevice())
		return NULL;

	return m_pSpuDecoder;
}

/**
 * Grabs the currently visible screen image
 *
 * @param size      size of the returned data
 * @param jpeg      flag true, create JPEG data
 * @param quality   JPEG quality
 * @param width     number of horizontal pixels in the frame
 * @param height    number of vertical pixels in the frame
 */
uchar *cSoftHdDevice::GrabImage(int &size, bool jpeg, int quality, int width, int height)
{
	if (!width || !height) {
		LOGWARNING("device: %s: width or height is 0 - skip!", __FUNCTION__);
		return nullptr;
	}

	if (IsDetached())
		return nullptr;

	if (m_pGrab->IsActive()) {
		LOGWARNING("device: %s: wait for the last grab to be finished - skip!", __FUNCTION__);
		return nullptr;
	}

	LOGDEBUG2(L_GRAB, "device: %s: %d, %d, %d, %dx%d", __FUNCTION__, size, jpeg, quality, width, height);

	if (!m_pGrab->Start(jpeg, quality, width, height, m_screenWidth, m_screenHeight))
		return nullptr;

	if (!m_pGrab->ProcessGrab())
		return nullptr;

	size = m_pGrab->Size();
	uchar *result = m_pGrab->Image();

	m_pGrab->Finish();

	return result;
}

/**
 * Sets the video display format
 *
 * @param videoDisplayFormat      video display format
 * Set it to the given one (only useful if this device has an MPEG decoder).
 */
void cSoftHdDevice::SetVideoDisplayFormat(eVideoDisplayFormat videoDisplayFormat)
{
	LOGDEBUG("device: %s: %d", __FUNCTION__, videoDisplayFormat);

	cDevice::SetVideoDisplayFormat(videoDisplayFormat);
}

/**
 * Set the video format
 *
 * Sets the output video format to either 16:9 or 4:3 (only useful
 * if this device has an MPEG decoder).
 *
 * Should call SetVideoDisplayFormat
 *
 * @param videoFormat16_9     flag true 16:9.
 */
void cSoftHdDevice::SetVideoFormat(bool videoFormat16_9)
{
	LOGDEBUG("device: %s: %d", __FUNCTION__, videoFormat16_9);

	// FIXME: 4:3 / 16:9 video format not supported.
	SetVideoDisplayFormat(eVideoDisplayFormat(Setup.VideoDisplayFormat));
}

/**
 * Get the video size
 *
 * Return the width, height and aspect ratio of the currently
 * displayed video material
 *
 * @param[out] width              video width
 * @param[out] height             video height
 * @param[out] aspectRatio        video aspect ratio
 *
 * @note the video_aspect is used to scale the subtitle.
 */
void cSoftHdDevice::GetVideoSize(int &width, int &height, double &aspectRatio)
{
//	LOGDEBUG("device: %s: %d x %d @ %f", __FUNCTION__, *width, *height, *aspectRatio);

	if (IsDetached()) { // return default values according to vdr docs
		width = 0;
		height = 0;
		aspectRatio = 1.0;
		return;
	}

	m_pVideoStream->GetVideoSize(&width, &height, &aspectRatio);
}

/**
 * Returns the width, height and aspect ratio the OSD should have
 *
 * @param[out] width              osd width
 * @param[out] height             osd height
 * @param[out] aspectRatio        osd aspect ratio
 *
 * @todo: Called every second, for nothing (no OSD displayed)?
 */
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &aspectRatio)
{
	if (IsDetached()) { // hardcode to 1920x1080 in detached state
		width = 1920;
		height = 1080;
		aspectRatio = (double)width / (double)height;
		return;
	}

	std::lock_guard<std::mutex> lock(m_sizeMutex);
	width = m_osdWidth;
	height = m_osdHeight;
	aspectRatio = (double)width / (double)height;
}

/**
 * Sets the audio volume on this device (Volume = 0...255).
 *
 * @param volume        device volume
 */
void cSoftHdDevice::SetVolumeDevice(int volume)
{
	if (IsDetached())
		return;

	LOGDEBUG("device: %s: %d", __FUNCTION__, volume);
	m_volume = volume;
	m_pAudio->SetVolume((volume * 1000) / 255);
}

/**
 * Return true if this device can currently start a replay session
 */
bool cSoftHdDevice::CanReplay(void) const
{
	bool canReplay = !IsDetached();

	LOGDEBUG("device: %s: %d", __FUNCTION__, canReplay);

	return canReplay;
}

/**
 * Sets the device into the given play mode.
 *
 * @param play_mode       new play mode (Audio/Video/External...)
 */
bool cSoftHdDevice::SetPlayMode(ePlayMode play_mode)
{
	LOGDEBUG("device: %s: %d", __FUNCTION__, play_mode);

	// A new play mode arrived, attach first if we did detach because of an external player
	if (m_externalPlayerActive) {
		TriggerEvent(AttachEvent{});
		m_externalPlayerActive = false;
	}

	switch (play_mode) {
	case pmNone:
		TriggerEvent(StopEvent{});
		break;
	case pmAudioVideo:
	case pmAudioOnly:
	case pmAudioOnlyBlack:
	case pmVideoOnly:
		TriggerEvent(PlayEvent{});
		break;
	case pmExtern_THIS_SHOULD_BE_AVOIDED:
		// External players like mpv (vdr-plugin-mpv) want to acquire DRM/ALSA
		// so we release it here and set a flag. As soon as the next SetPlayMode arrives
		// we then can attach again before changing to the new playmode.
		m_pPipHandler->Disable();
		TriggerEvent(DetachEvent{});
		m_externalPlayerActive = true;
		break;
	default:
		LOGERROR("device: %s: playmode not supported %d", play_mode);
		return false;
		break;
	}

	return true;
}

/**
 * Play an audio packet
 *
 * This is the main function, which is called by VDR to play audio data
 *
 * @param data   data of exactly one complete PES packet
 * @param size   size of PES packet
 * @param id     PES packet type
 *
 * The caller must ensure, that PlayAudio() is not called in detached state.
 * (CanReplay() and HasDecoder() return false in this state and we are not
 * the primary device.)
 */
int cSoftHdDevice::PlayAudio(const uchar *data, int size, uchar id)
{
//	LOGDEBUG("device: %s: %p %p %d %d", __FUNCTION__, this, data, size, id);
	if (IsDetached())
		return size;

	m_receivedAudio = true;

	if (m_pAudio->IsBufferFull())
		return 0;

	cPesAudio pesPacket((const uint8_t*)data, size);

	if (!pesPacket.IsValid()) {
		m_audioReassemblyBuffer.Reset();

		return size;
	}

	if (!m_receivedValidAudio && Transferring()) {
		auto now = std::chrono::steady_clock::now();
		auto timeUntilFirstPacketReceived = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_channelSwitchStartTime).count();
		LOGDEBUG("device: first valid audio packet arrives %dms after channel switch was triggered", timeUntilFirstPacketReceived);

		if (!m_receivedValidVideo)
			m_dataReceivedTime = now;
	}
	m_receivedValidAudio = true;

	if (Transferring()) { // compensation is only necessary with live streams
		m_pAudio->ClockDriftCompensation();
		m_audioJitterTracker.PacketReceived();
		m_pConfig->StatMaxShortTermAudioJitterMs = m_audioJitterTracker.GetShortTermMaxJitterMs();
		m_pConfig->StatMaxLongTermAudioJitterMs = m_audioJitterTracker.GetLongTermMaxJitterMs();
	}

	if (m_audioChannelID != id) {
		m_audioChannelID = id;
		m_audioReassemblyBuffer.Reset();
		m_pAudioDecoder->Close();
		LOGDEBUG("device: %s: new channel id 0x%02X", __FUNCTION__, m_audioChannelID);
	}

	m_audioReassemblyBuffer.Push(pesPacket.GetPayload(), pesPacket.GetPayloadSize(), pesPacket.GetPts());

	if (IsBufferingThresholdReached())
		TriggerEvent(BufferingThresholdReachedEvent{});

	AVPacket *avpkt;
	do {
		if (!(avpkt = m_audioReassemblyBuffer.PopAvPacket()))
			break;

		if (m_pAudioDecoder->GetCodecId() == AV_CODEC_ID_NONE && m_audioReassemblyBuffer.GetCodec() != AV_CODEC_ID_NONE) {
			// The playback has just started
			m_pAudioDecoder->Close();
			m_pAudioDecoder->Open(m_audioReassemblyBuffer.GetCodec());
		}

		m_pAudioDecoder->Decode(avpkt);
		AVPacket *copy = avpkt;
		av_packet_free(&copy);
	} while (avpkt != nullptr);

	return size;
}

/**
 * Play a video packet of the main videostream
 *
 * This is the main function, which is called by VDR to play video data
 *
 * @param data    A complete PES packet with optionally fragmented payload
 * @param size    the length of the PES packet including header
 *
 * This is called directly from VDR
 *
 * The caller must ensure, that PlayVideo() is not called in detached state.
 * (CanReplay() and HasDecoder() return false in this state and we are not
 * the primary device.)
 */
int cSoftHdDevice::PlayVideo(const uchar *data, int size)
{
//	LOGDEBUG("device: %s: %p %d", __FUNCTION__, data, size);
	if (IsDetached())
		return size;

	m_receivedVideo = true;

	return PlayVideoInternal(m_pVideoStream, &m_videoReassemblyBuffer, data, size, Transferring(), true);
}

/**
 * Gets the current System Time Counter, which can be used to
 * synchronize audio, video and subtitles.
 */
int64_t cSoftHdDevice::GetSTC(void)
{
	if (IsDetached())
		return AV_NOPTS_VALUE;

	switch (m_playbackMode) {
		case NONE:
			return AV_NOPTS_VALUE;
		case AUDIO_AND_VIDEO:
		case VIDEO_ONLY:
			return m_pRender->GetVideoClock();
		case AUDIO_ONLY:
			return m_pAudio->GetHardwareOutputPtsTimebaseUnits();
	}

	abort();
}

/**
 * Ask the output, if it can scale video
 *
 * @param rect      requested video window rectangle
 *
 * @return          the real rectangle or cRect::NULL if invalid
 */
cRect cSoftHdDevice::CanScaleVideo(const cRect & rect, __attribute__ ((unused)) int alignment)
{
	if (m_screenWidth == m_osdWidth && m_screenHeight == m_osdHeight)
		return rect;

	double scaleFactor = std::min((double)m_screenWidth / m_osdWidth, (double)m_screenHeight / m_osdHeight);
	int width  = std::lround(scaleFactor * rect.Width());
	int height = std::lround(scaleFactor * rect.Height());
	int x      = std::lround(scaleFactor * rect.X());
	int y      = std::lround(scaleFactor * rect.Y());

	x = std::max(0, x);
	y = std::max(0, y);
	if (x + width > m_screenWidth)
		width = m_screenWidth - x;
	if (y + height > m_screenHeight)
		height = m_screenHeight - y;

	if (width <= 0 || height <= 0)
		return cRect::Null;

	LOGDEBUG2(L_DRM, "device: %s: scale rect %dx%d-%d|%d -> %dx%d-%d|%d", __FUNCTION__,
		rect.Width(), rect.Height(), rect.X(), rect.Y(), width, height, x, y);

	return cRect(x, y, width, height);
}

/**
 * Scale the currently shown video
 *
 * @param x         video window x coordinate OSD relative
 * @param y         video window x coordinate OSD relative
 * @param width     video window width OSD relative
 * @param height    video window height OSD relative
 */
void cSoftHdDevice::ScaleVideo(const cRect & rect)
{
	if (IsDetached())
		return;

	LOGDEBUG2(L_OSD, "device: %s: %dx%d%+d%+d",
		__FUNCTION__, rect.Width(), rect.Height(), rect.X(), rect.Y());

	if (m_pRender)
		m_pRender->SetVideoOutputPosition(rect);
}

/**
 * Sets the device into a mode where replay is done slower.
 * Every single frame shall then be displayed the given number of
 * times. Forward is true if replay is done in the normal (forward)
 * direction, false if it is done reverse.
 * The cDvbPlayer uses the following values for the various speeds:
 *                   1x   2x   3x
 * Fast Forward       6    3    1
 * Fast Reverse       6    3    1
 * Slow Forward       8    4    2
 * Slow Reverse      63   48   24
 */
void cSoftHdDevice::TrickSpeed(int speed, bool forward)
{
	LOGDEBUG("device: %s: %d %s", __FUNCTION__, speed, forward ? "forward" : "backward");

	// This normalizes the VDR frame displaying count into a factor, representing how fast/slow the playback shall be.
	// For example, a factor of 2.0 means twice as fast as normal, a factor of 0.5 means half as fast as normal (slow-mo).
	// This is necessary because VDR sends only I-frames during trickspeed, but the distance between I-frames depends on the encoding parameters.
	// Therefore, we send a normalized factor for the further components, which then calculate the necessary frame displaying count by considering the distance between I-frames.

	double normalizedSpeed = 1;
	static constexpr double MAX_SPEED = 3;

	// these are arbitrary values, which feel just right
	static constexpr double FAST_TRICKSPEED_FACTOR = 5; // the higher the factor, the faster the fast forward/reverse
	static constexpr double SLOW_FORWARD_FACTOR = 2; // the higher the factor, the slower the slow-mo

	// Fastest speed in reverse slow-mo is the original speed. Slower speeds are too slow, because of the already low frame rate.
	static constexpr double SLOW_REVERSE_FACTOR = 1;

	// speed of the trickspeed (VDR's magic frame displaying count)
	switch (speed) {
		case 6:
		case 8:
		case 63:
			normalizedSpeed = 1; // slowest (both, in fast trickspeed and slow-mo)
			break;
		case 3:
		case 4:
		case 48:
			normalizedSpeed = 2;
			break;
		case 1:
		case 2:
		case 24:
			normalizedSpeed = 3; // fastest (both, in fast trickspeed and slow-mo)
			break;
	}

	// figure out if VDR demands slow-mo or fast trickspeed
	double tmp;
	switch (speed) {
		case 8:
		case 4:
		case 2:
		case 63:
		case 48:
		case 24:
			// slow-mo
			tmp = (MAX_SPEED + 1) - normalizedSpeed;

			if (forward)
				tmp *= SLOW_FORWARD_FACTOR;
			else
				tmp *= SLOW_REVERSE_FACTOR;

			normalizedSpeed = 1 / tmp;
		break;
		default:
			// fast trickspeed
			normalizedSpeed *= FAST_TRICKSPEED_FACTOR;
		break;
	}

	TriggerEvent(TrickSpeedEvent{normalizedSpeed, speed != 0, forward});
}

/**
 * Clears all video and audio data from the device.
 *
 * This is called by VDR via DeviceClear() in the Empty() call.
 *
 * Empty() does clear all VDR internal packets.
 */
void cSoftHdDevice::Clear(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	cDevice::Clear();

	if (IsDetached())
		return;

	m_pRender->Halt();
	m_pVideoStream->Halt();

	m_pRender->SetDisplayOneFrameThenPause(true);
	m_pVideoStream->CancelFilterThread();

	m_videoReassemblyBuffer.Reset();
	m_pVideoStream->ClearVdrCoreToDecoderQueue();
	m_pRender->ClearDecoderToDisplayQueue();

	if (m_playbackMode == AUDIO_AND_VIDEO || m_playbackMode == VIDEO_ONLY)
		m_pVideoStream->FlushDecoder();

	m_pRender->Reset();

	m_pAudio->SetPaused(true);
	m_pAudio->ResetHwDelayBaseline();
	FlushAudio();

	m_pStateMachine->SetState(BUFFERING);

	m_pRender->Resume();
	m_pVideoStream->Resume();
}

/**
 * Sets the device into play mode (after a previous trick mode, or pause)
 *
 * This is called by VDR via DevicePlay() in the Play() and Goto() call
 */
void cSoftHdDevice::Play(void)
{
	cDevice::Play();

	TriggerEvent(PlayEvent{});
}

/**
 * Puts the device into "freeze frame" mode.
 */
void cSoftHdDevice::Freeze(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	cDevice::Freeze();

	TriggerEvent(PauseEvent{});
}

/**
 * Display the given I-frame as a still picture.
 *
 * @param data       pes or ts data of a frame
 * @param length     length of data area
 */
void cSoftHdDevice::StillPicture(const uchar *data, int size)
{
	LOGDEBUG("device: %s: %s %p %d", __FUNCTION__, data[0] == 0x47 ? "ts" : "pes", data, size);

	if (data[0] == 0x47) {		// ts sync byte
		cDevice::StillPicture(data, size);
		return;
	}

	TriggerEvent(StillPictureEvent{data, size});
}

/**
 * Return true if the device itself or any of the file handles in
 * poller is ready for further action.
 * If TimeoutMs is not zero, the device will wait up to the given number
 * of milliseconds before returning in case it can't accept any data.
 *
 * @param poller        file handles (unused)
 * @param timeoutMs     timeout in ms to become ready
 *
 * @retval true         if ready
 * @retval false        if busy
 */
bool cSoftHdDevice::Poll(__attribute__ ((unused)) cPoller & poller, int timeoutMs)
{
//	LOGDEBUG("device: %s: timeout %d", __FUNCTION__, timeout_ms);
	if (IsDetached())
		return true;

	if (!m_pAudio->IsBufferFull() && !m_pVideoStream->IsInputBufferFull())
		return true;

	usleep(timeoutMs * 1000);

	return false;
}

/**
 * Return true, if the output buffers are empty, false otherwise.
 * Wait max. up to timeoutMs in case the buffers are not empty.
 *
 * This function does not initiate a decoder drain like Drain()
 * so some data may stay unprocessed in the decoder, while the other
 * buffers are already emtpy. Therefore, players should use the
 * new Drain() function instead.
 *
 * @param timeoutMs        timeout in ms to become ready
 *
 * @return true, if the buffers are empty, false otherwise
 *
 * @note Flush() is marked DEPRECATED since APIVERSION 14
 */
bool cSoftHdDevice::Flush(int timeoutMs)
{
	if (IsDetached())
		return true;

	LOGDEBUG("device: %s: timeout % ms", __FUNCTION__, timeoutMs);

	const auto buffersEmpty = [&]() {
		return m_playbackMode == AUDIO_ONLY
			? m_pAudio->IsBufferEmpty()
			: m_pVideoStream->BuffersEmpty();
	};

	const cTimeMs timeout(timeoutMs);
	while (!buffersEmpty() && !timeout.TimedOut())
		cCondWait::SleepMs(std::min(5, timeoutMs));

	return buffersEmpty();
}

#if APIVERSNUM >= 30014
/**
 * Force a decoder drain and return true, if all buffers have been played out
 *
 * @return true, if the buffers are empty, false otherwise
 */
bool cSoftHdDevice::Drain(void)
{
	if (IsDetached())
		return true;

	// enter drain mode once
	if (!m_draining) {
		LOGDEBUG("device: %s: start draining", __FUNCTION__);
		m_draining = true;
		if (!m_videoReassemblyBuffer.IsEmpty())
			m_pVideoStream->PushAvPacket(m_videoReassemblyBuffer.PopAvPacket());
		m_pVideoStream->Drain();
	}

	const auto buffersEmpty = [&]() {
		return m_playbackMode == AUDIO_ONLY
			? m_pAudio->IsBufferEmpty()
			: m_pVideoStream->BuffersEmpty();
	};

	if (!buffersEmpty())
		return false;

	LOGDEBUG("device: %s: drained, buffers are empty", __FUNCTION__);
	m_draining = false;

	return true;
}
#endif

/*********************************************************************
 * VDR cStatus interface
 ********************************************************************/

/**
 * Monitor a channel switch triggered by VDR (cStatus::ChannelSwitch())
 *
 * Save the timestamp when a channel switch is initiated (channelNum == 0)
 * for later time measurement.
 */
void cSoftHdDevice::ChannelSwitch(const cDevice *device, int channelNum, bool liveView)
{
	if (device != cDevice::PrimaryDevice())
		return;

	if (!liveView)
		return;

	if (channelNum == 0)
		m_channelSwitchStartTime = std::chrono::steady_clock::now();
}

/*********************************************************************
 * cSoftHdDevice public API - playback, display, decoder control
 ********************************************************************/

/**
 * Disables deinterlacer (called from setup menu or conf)
 */
void cSoftHdDevice::SetDisableDeint(void)
{
	if (m_pVideoStream)
		m_pVideoStream->DisableDeint(m_pConfig->ConfigDisableDeint);
}

/**
 * Forces the h264 decoder to wait for an I-Frame to start
 */
void cSoftHdDevice::SetDecoderNeedsIFrame(void)
{
	if (m_pVideoStream)
		m_pVideoStream->SetStartDecodingWithIFrame(m_pConfig->ConfigDecoderNeedsIFrame);
}

/**
 * Parse the h264 stream width and height before starting the decoder
 */
void cSoftHdDevice::SetParseH264Dimensions(void)
{
	if (m_pVideoStream)
		m_pVideoStream->SetParseH264Dimensions(m_pConfig->ConfigParseH264Dimensions);
}

/**
 * Force the decoder to fallback to software if the hardware decoder fails
 * after the configured amount of packets were sent and no frame was received
 */
void cSoftHdDevice::SetDecoderFallbackToSw(bool enable)
{
	if (!m_pVideoStream)
		return;

	if (enable)
		m_pVideoStream->SetDecoderFallbackToSwNumPkts(m_pConfig->ConfigDecoderFallbackToSwNumPkts);
	else
		m_pVideoStream->SetDecoderFallbackToSwNumPkts(0);
}

/**
 * Enable HDR display mode
*/
void cSoftHdDevice::SetEnableHdr(bool enable)
{
    m_pRender->SetEnableHdr(enable);
};

/**
 * Trigger a display mode change event if the mode changed
 *
 * @param idx     setup menu array index of the mode
 */
void cSoftHdDevice::SetDisplayMode(int idx)
{
	sDrmMode *mode = &m_pConfig->AutoDetectedDrmMode;

	if (idx == CONFIG_DISPLAY_MODE_FOLLOW_VIDEO ||
	    idx == CONFIG_DISPLAY_MODE_FOLLOW_VIDEO_INTERLACED) {
		mode = &m_pConfig->CurrentVideoDrmMode;
		if (!mode->width || !m_pRender->CanHandleMode(mode))
			mode = &m_pConfig->AutoDetectedDrmMode;
	} else if (idx >= CONFIG_DISPLAY_MODE_MANUAL)
		mode = &m_pConfig->CollectedDrmModes[idx - CONFIG_DISPLAY_MODE_MANUAL];

	// Check, if the requested mode differs from the current one at all
	if (!m_pConfig->CompareCurrentMode(mode)) {
		LOGDEBUG("Add display mode change event to %s mode %dx%d@%.2f%s",
			 idx == CONFIG_DISPLAY_MODE_DEFAULT ? "default" :
			(idx == CONFIG_DISPLAY_MODE_FOLLOW_VIDEO ? "follow video" :
			(idx == CONFIG_DISPLAY_MODE_FOLLOW_VIDEO_INTERLACED ? "follow video interlaced" :
			 "fixed")),
			mode->width, mode->height, mode->refreshRateHz, mode->interlaced ? "i" : "");
		m_pEventHandler->AddEvent(DisplayChangeEvent{*mode});
	}
}

/**
 * Check if the buffering threshold has been reached
 *
 * During the BUFFERING state, this method determines when sufficient audio/video data
 * has been buffered to start playback.
 *
 * ThresholdReached (Sync-Ability) is signalled
 *   1) in audio only mode:
 *      -> PlayAudio() was called
 *      -> audio input has a valid pts
 *      -> enough audio data is buffered
 *   2) in video only mode:
 *      -> PlayVideo() was called
 *      -> video input has a valid pts
 *      -> enough video data is buffered (which implies "a video frame reached the renderer")
 *   3) in audio/video mode:
 *      -> audio input has a valid pts
 *      -> video input has a valid pts
 *      -> video and audio has enough data buffered (calculated from the first output pts to play)
 *      -> the render output buffer queue is completely filled once (which implies "a video frame reached the renderer")
 *
 * @retval true    if playback should start (audio or video only or buffering threshold reached)
 * @retval false   if playback should not start
 *
 * @note In order to signal ThresholdReached, both (audio and video) need to have a valid pts in audio + video mode!
 */
bool cSoftHdDevice::IsBufferingThresholdReached()
{
	if (m_pStateMachine->GetState() != BUFFERING)
		return false;

	bool audioHasInputPts = m_pAudio->HasInputPts();
	bool videoHasInputPts = m_pVideoStream->HasInputPts();
	bool videoHasOutputPts = m_pRender->GetOutputPtsMs() != AV_NOPTS_VALUE;

	// Assume audio only or video only if no PES fragment from the other stream has been received, while the buffering threshold of the other stream is reached.
	// Check for buffer fill level only if at least one PES packet was reassembled and pushed to the respective decoder.
	bool audioOnly = audioHasInputPts && !videoHasInputPts && m_receivedAudio && !m_receivedVideo;
	bool videoOnly = !audioHasInputPts && videoHasInputPts && !m_receivedAudio && m_receivedVideo;

	if        (audioOnly &&                      m_pAudio->GetInputPtsMs() - m_pAudio->GetOutputPtsMs() > GetBufferFillLevelThresholdMs()) {
		LOGDEBUG("device: %s: Detected audio only", __FUNCTION__);
		return true;
	} else if (videoOnly && videoHasOutputPts && m_pVideoStream->GetInputPtsMs() - m_pRender->GetOutputPtsMs() > GetBufferFillLevelThresholdMs()) {
		LOGDEBUG("device: %s: Detected video only", __FUNCTION__);
		return true;
	} else if (!audioHasInputPts || !videoHasInputPts || !videoHasOutputPts)
		return false; // Either no video or no audio received, yet. Or, video didn't make it to the output buffer, yet.

	int64_t syncedAudioBufferFillLevelMs = m_pAudio->GetInputPtsMs() - GetFirstAudioPtsMsToPlay();
	int64_t syncedVideoBufferFillLevelMs = m_pVideoStream->GetInputPtsMs() - GetFirstVideoPtsMsToPlay();

	bool reached = m_pRender->IsOutputBufferFull() && // video decoder output buffer (audio hardware output buffer is negligible)
		syncedVideoBufferFillLevelMs > GetBufferFillLevelThresholdMs() && // video decoder input buffer
		syncedAudioBufferFillLevelMs > GetBufferFillLevelThresholdMs(); // audio decoder output buffer

	if (reached) {
		LOGDEBUG2(L_AV_SYNC, "First received PTS: %s (audio), %s (video) buffer fill levels: %ldms (audio) %ldms (video)",
			Timestamp2String(m_pAudio->GetOutputPtsMs(), 1),
			Timestamp2String(m_pRender->GetOutputPtsMs(), 1),
			syncedAudioBufferFillLevelMs,
			syncedVideoBufferFillLevelMs);
	}

	return reached;
}

/*********************************************************************
 * cSoftHdDevice public API - osd control
 ********************************************************************/

#ifdef USE_GLES
/**
 * Get the maximum GPU image cache size
 */
int cSoftHdDevice::MaxSizeGPUImageCache(void)
{
	return m_pConfig->ConfigMaxSizeGPUImageCache;
};

/**
 * Is the OpenGL/ES osd disabled?
 */
int cSoftHdDevice::OglOsdIsDisabled(void)
{
	return m_pConfig->ConfigDisableOglOsd;
};

/**
 * Disables OpenGL/ES Osd (called from setup menu or conf)
 */
void cSoftHdDevice::SetDisableOglOsd(void)
{
	m_pConfig->ConfigDisableOglOsd = 1;
	if (m_pRender)
		m_pRender->DisableOglOsd();
}

/**
 * Enables OpenGL/ES Osd
 */
void cSoftHdDevice::SetEnableOglOsd(void)
{
	m_pConfig->ConfigDisableOglOsd = 0;
	if (m_pRender)
		m_pRender->EnableOglOsd();
}
#endif

/**
 * Close the OSD
 */
void cSoftHdDevice::OsdClose(void)
{
	if (IsDetached())
		return;

	m_pRender->OsdClear();
}

/**
 * Draw an OSD pixmap
 *
 * @param xi         x-coordinate in argb image
 * @param yi         y-coordinate in argb image
 * @param height     height in pixel in argb image
 * @param width      width in pixel in argb image
 * @param pitch      pitch of argb image
 * @param argb       32bit ARGB image data
 * @param x          x-coordinate on screen of argb image
 * @param y          y-coordinate on screen of argb image
 */
void cSoftHdDevice::OsdDrawARGB(int xi, int yi, int height, int width, int pitch,
	const uint8_t * argb, int x, int y)
{
	if (IsDetached())
		return;

	m_pRender->OsdDrawARGB(xi, yi, height, width, pitch, argb, x, y);
}

/**
 * Set the OSD size
 *
 * @param width           osd width
 * @param height          osd height
 */
void cSoftHdDevice::SetOsdSize(int width, int height)
{
	std::lock_guard<std::mutex> lock(m_sizeMutex);
	m_osdWidth = width;
	m_osdHeight = height;
}

/**
 * Set the screen size
 *
 * @param width           screen width
 * @param height          screen height
 */
void cSoftHdDevice::SetScreenSize(int width, int height)
{
	std::lock_guard<std::mutex> lock(m_sizeMutex);
	m_screenWidth = width;
	m_screenHeight = height;
}

/*********************************************************************
 * cSoftHdDevice public API - audio control
 ********************************************************************/

/**
 * Set the passthrough mask (called from setup menu or conf)
 */
void cSoftHdDevice::SetPassthroughMask(int mask)
{
	m_pAudio->SetPassthroughMask(mask);
	if (m_pAudioDecoder)
		m_pAudioDecoder->SetPassthroughMask(mask);
}

/**
 * Reset the channel ID (restarts audio)
 */
void cSoftHdDevice::ResetChannelId(void)
{
	LOGDEBUG("%s:", __FUNCTION__);
	m_audioChannelID = -1;
}

/*********************************************************************
 * cSoftHdDevice public API - logging, statistics
 ********************************************************************/

/**
 * Get statistics from the renderer
 *
 * @param[out] duped     duped frames
 * @param[out] dropped   dropped frames
 * @param[out] count     number of total rendered frames
 */
void cSoftHdDevice::GetStats(int *duped, int *dropped, int *counter)
{
	*duped = 0;
	*dropped = 0;
	*counter = 0;
	if (m_pRender) {
		m_pRender->GetStats(duped, dropped, counter);
	}
}

/*********************************************************************
 * cSoftHdDevice public API - mediaplayer
 ********************************************************************/

/**
 * Open an audio codec
 *
 * @param codecId       audio codec id
 * @param par           audio codec parameters
 * @param timebase      timebase
 */
void cSoftHdDevice::SetAudioCodec(enum AVCodecID codecId, AVCodecParameters * par, AVRational timebase)
{
	m_pAudioDecoder->Open(codecId, par, timebase);
}

/**
 * Open a video codec
 *
 * @param codecId       video codec id
 * @param par           video codec parameters
 * @param timebase      timebase
 */
void cSoftHdDevice::SetVideoCodec(enum AVCodecID codecId, AVCodecParameters * par, AVRational timebase)
{
	m_pVideoStream->Open(codecId, par, timebase);
}

/**
 * Play an audio packet
 *
 * @param pkt        AVPacket to play
 *
 * @retval 0         packet could not be player, free audio buffer too small
 * @retval 1         packet was sent to be decoded
 */
int cSoftHdDevice::PlayAudioPkts(AVPacket * pkt)
{
	m_pAudio->LazyInit();

	if (m_pAudio->IsBufferFull())
		return 0;

	m_pAudioDecoder->Decode(pkt);
	return 1;
}

/**
 * Play a video packet
 *
 * @param pkt        AVPacket to play
 *
 * @retval 0         packet could not be player, free audio buffer too small
 * @retval 1         packet was sent to be decoded
 */
int cSoftHdDevice::PlayVideoPkts(AVPacket * pkt)
{
	m_pAudio->LazyInit();

	if (m_pVideoStream->GetAvPacketsFilled() >= (size_t)m_pVideoStream->GetVideoPacketMax() - 10)
		return 0;

	m_pVideoStream->PushAvPacket(pkt);

	return 1;
}

/*********************************************************************
 * cSoftHdDevice public API - detach/ attach
 ********************************************************************/

/**
 * Detach the device
 *
 * Clears audio and video, stops all threads and releases drm/alsa.
 * A detached state can only be exited (restarted) with an AttachEvent.
 */
void cSoftHdDevice::Detach(void)
{
	if (cDevice::Replaying()) {
		LOGDEBUG("device: %s: Device is replaying, stop replay first", __FUNCTION__);
		StopReplay();
	}

	if (IsPrimaryDevice(false)) {
		m_needsMakePrimary = true;
		MakePrimaryDevice(false);
	}

	m_pPipHandler->Disable();
	TriggerEvent(DetachEvent{});
}

/**
 * Attach the device again
 *
 * Kind of a plugin restart. Inits and starts all necessary resources.
 * Only valid after a detach.
 */
void cSoftHdDevice::Attach(void)
{
	m_forceDetached = false;

	if (m_needsMakePrimary) {
		MakePrimaryDevice(true);
		m_needsMakePrimary = false;
	}

	TriggerEvent(AttachEvent{});
}

/**
 * Returns true, if the device is detached
 */
bool cSoftHdDevice::IsDetached(void) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_pStateMachine->GetState() == State::DETACHED;
}

/*********************************************************************
 * cSoftHdDevice public API - pip
 ********************************************************************/

/**
 * Play a video packet of the pip videostream
 *
 * @param data    A complete PES packet with optionally fragmented payload
 * @param size    the length of the PES packet including header
 *
 * The caller must ensure, that PlayPipVideo() is not called in detached state.
 * (CanReplay() and HasDecoder() return false in this state and we are not
 * the primary device.)
 */
int cSoftHdDevice::PlayPipVideo(const uchar *data, int size)
{
//	LOGDEBUG("device: %s: %p %d", __FUNCTION__, data, size);

	// This is a bit hacky:
	// Because we do no sync with the pip stream, we simply drop data
	// if the input buffer is full -> this prevents us from having a buffer overflow
	// caused by the pip stream.
	if (m_pPipStream->IsInputBufferFull())
		return size;

	return PlayVideoInternal(m_pPipStream, &m_pipReassemblyBuffer, data, size, false, false);
}

/**
 * Resets pip stream and render pipeline
 */
void cSoftHdDevice::ResetPipStream(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	m_pPipStream->Halt();

	m_pPipStream->CancelFilterThread();
	m_pipReassemblyBuffer.Reset();
	m_pPipStream->ClearVdrCoreToDecoderQueue();
	m_pPipStream->CloseDecoder();

	m_pRender->Halt();

	m_pRender->ClearPipDecoderToDisplayQueue();
	m_pRender->ResetPipDecodingStrategy();
	m_pRender->ResetPipBufferReuseStrategy();

	m_pRender->Resume();
	m_pPipStream->Resume();
}

/**
 * Returns true, if pip is currently enabled
 */
bool cSoftHdDevice::PipIsEnabled(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_pPipHandler->IsEnabled();
}

void cSoftHdDevice::PipEnable(void) { m_pPipHandler->Enable(); };
void cSoftHdDevice::PipDisable(void) { m_pPipHandler->Disable(); };
void cSoftHdDevice::PipToggle(void) { m_pPipHandler->Toggle(); };
void cSoftHdDevice::PipChannelChange(int dir) { m_pPipHandler->ChannelChange(dir); };
void cSoftHdDevice::PipChannelSwap(bool closePip) { m_pPipHandler->ChannelSwap(closePip); };
void cSoftHdDevice::PipSwapPosition(void) { m_pPipHandler->SwapPosition(); };
void cSoftHdDevice::PipSetSize(void) { m_pPipHandler->SetSize(); };

/*
 * Wrapper functions for cVideoRender and cPipHandler
 */
void cSoftHdDevice::SetRenderPipSize(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_pRender->Halt();
	m_pRender->SetPipSize(m_pipUseAlt);
	m_pRender->Resume();
};

void cSoftHdDevice::SetRenderPipActive(bool active)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_pRender->Halt();
	m_pRender->SetPipActive(active);
	m_pRender->Resume();
};

/*********************************************************************
 * cSoftHdDevice private functions
 ********************************************************************/

/**
 * Print the start code, stream id, length, first three bytes (start code) of the payload, and the following 16 bytes of the codec payload.
 *
 * @param data        pointer to stream data
 * @param offset      print from here
 *
 * @ingroup device
 */
static void PrintStreamData(const uchar *payload)
{
	LOGDEBUG2(L_CODEC, "Stream: %02X%02X%02X | %02X | %02X%02X | %02X%02X%02X | %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
		payload[0],
		payload[1],
		payload[2],
		payload[3],
		payload[4],
		payload[5],
		payload[6],
		payload[7],
		payload[8],
		payload[9],
		payload[10],
		payload[11],
		payload[12],
		payload[13],
		payload[14],
		payload[15],
		payload[16],
		payload[17],
		payload[18],
		payload[19],
		payload[20],
		payload[21],
		payload[22],
		payload[23],
		payload[24]
	);
}

/**
 * Play a video packet
 *
 * @param stream          video stream to play to
 * @param buffer          reassembly buffer for this stream
 * @param data            A complete PES packet with optionally fragmented payload
 * @param size            the length of the PES packet including header
 * @param trackJitter     whether to track jitter for this packet
 * @param mainStream      this is a packet of the main stream
 */
int cSoftHdDevice::PlayVideoInternal(cVideoStream *stream, cReassemblyBufferVideo *buffer, const uchar *data, int size, bool trackJitter, bool mainStream)
{
	// LOGDEBUG("device: %s: %p %d %s", __FUNCTION__, data, size, mainStream ? "video" : "pip");

	if (stream->IsInputBufferFull())
		return 0;

	cPesVideo pesPacket((const uint8_t*)data, size);

	if (!pesPacket.IsValid()) {
		buffer->Reset();

		return size;
	}

	if (mainStream) {
		if (!m_receivedValidVideo && Transferring()) {
			auto now = std::chrono::steady_clock::now();
			auto timeUntilFirstPacketReceived = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_channelSwitchStartTime).count();
			LOGDEBUG("device: first valid video packet arrives %dms after channel switch was triggered", timeUntilFirstPacketReceived);

			if (!m_receivedValidAudio)
				m_dataReceivedTime = now;

		}
		m_receivedValidVideo = true;
	}

	if (trackJitter) {
		m_videoJitterTracker.PacketReceived();
		m_pConfig->StatMaxShortTermVideoJitterMs = m_videoJitterTracker.GetShortTermMaxJitterMs();
		m_pConfig->StatMaxLongTermVideoJitterMs = m_videoJitterTracker.GetLongTermMaxJitterMs();
	}

	if (stream->GetCodecId() == AV_CODEC_ID_NONE) {
		// The playback has just started
		if (!pesPacket.HasPts() || !buffer->ParseCodecHeader(pesPacket.GetPayload(), pesPacket.GetPayloadSize())) {
			// received the middle of fragmented data, wait for the next PES packets with the start of a new frame
			return size;
		}

		PrintStreamData(data);
		buffer->Push(pesPacket.GetPayload(), pesPacket.GetPayloadSize(), pesPacket.GetPts());

		stream->Open(buffer->GetCodec());
	} else {
		int payloadOffset = 0;
		if (pesPacket.HasPts() && !buffer->IsEmpty()) {
			// received the first fragment of a new frame, finish the current reassembly buffer into an AVPacket
			stream->PushAvPacket(buffer->PopAvPacket());

			// populate the cleared buffer with the next frame
			if (buffer->HasLeadingZero(pesPacket.GetPayload(), pesPacket.GetPayloadSize()))
				payloadOffset = 1; // H.264/HEVC streams may have a leading zero byte before the start code
		}

		buffer->Push(pesPacket.GetPayload() + payloadOffset, pesPacket.GetPayloadSize() - payloadOffset, pesPacket.GetPts());
	}

	return size;
}

/**
 * Clear all audio data from the decoder and ringbuffer
 */
void cSoftHdDevice::FlushAudio(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	m_pAudioDecoder->FlushBuffers();
	m_pAudio->FlushBuffers();
	m_audioReassemblyBuffer.Reset();
}

/**
 * The still picture data received from VDR can contain multiple PES packets.
 * This sends each PES packet's raw data separately to PlayVideo(), and does a flush to display the frame immediately.
 *
 * @param data       pes data of one or more frames
 * @param size       length of data area
 */
void cSoftHdDevice::HandleStillPicture(const uchar *data, int size)
{
	m_pRender->SetPlaybackPaused(false);
	m_pVideoStream->SetDeinterlacerDeactivated(true);

	// skip BufferUnderrunEvent{VIDEO} in renderer
	m_pRender->SetStillpicture(true);

	const uchar *currentPacketStart = data;
	while (currentPacketStart < data + size) {
		cPesVideo pesPacket((const uint8_t*)currentPacketStart, size - (currentPacketStart - data));

		if (pesPacket.IsValid())
			PlayVideoInternal(m_pVideoStream, &m_videoReassemblyBuffer, currentPacketStart, pesPacket.GetPacketLength(), false, true);
		else {
			LOGWARNING("device: %s: invalid PES packet", __FUNCTION__);
			break;
		}

		currentPacketStart += pesPacket.GetPacketLength();
	}

	m_pVideoStream->PushAvPacket(m_videoReassemblyBuffer.PopAvPacket());
	m_pVideoStream->ResetInputPts(); // stillpicture shouldn't trigger having video data
	m_pVideoStream->Drain();
}

/**
 * Set the display mode
 *
 * @param mode     drm mode
 */
void cSoftHdDevice::HandleDisplayModeChange(const sDrmMode &mode)
{
	LOGDEBUG("Set display mode: %dx%d@%.2f%s",
		mode.width,
		mode.height,
		mode.refreshRateHz,
		mode.interlaced ? "i" : "");

	m_pConfig->RequestedDrmMode = mode;
	m_pRender->ReInitDisplayMode();
}

/**
 * Calculate the first audio PTS that should be played during synchronized playback
 *
 * This method determines the starting audio presentation timestamp when transitioning
 * from BUFFERING to PLAY state. It synchronizes audio with video by taking the maximum
 * of both output PTSes, then adjusts for user-configured audio/video delay.
 *
 * @return the first audio PTS in milliseconds that should be played
 *
 * @note Positive ConfigVideoAudioDelayMs means audio is intentionally delayed (video ahead)
 * @note Negative ConfigVideoAudioDelayMs means video is intentionally delayed (audio ahead)
 */
int64_t cSoftHdDevice::GetFirstAudioPtsMsToPlay()
{
	int64_t ret = std::max(m_pRender->GetOutputPtsMs(), m_pAudio->GetOutputPtsMs());

	if (m_pConfig->ConfigVideoAudioDelayMs < 0)
		ret -= m_pConfig->ConfigVideoAudioDelayMs;

	return ret;
}

/**
 * @see cSoftHdDevice::GetFirstAudioPtsMsToPlay()
 */
int64_t cSoftHdDevice::GetFirstVideoPtsMsToPlay()
{
	int64_t ret = std::max(m_pRender->GetOutputPtsMs(), m_pAudio->GetOutputPtsMs());

	if (m_pConfig->ConfigVideoAudioDelayMs > 0)
		ret += m_pConfig->ConfigVideoAudioDelayMs;

	return ret;
}

/**
 * Returns the buffer fill level threshold in milliseconds.
 * Combines the minimum threshold with the user-configured additional buffer length.
 */
int cSoftHdDevice::GetBufferFillLevelThresholdMs() {
	return MIN_BUFFER_FILL_LEVEL_THRESHOLD_MS + m_pConfig->ConfigAdditionalBufferLengthMs;
}

/*********************************************************************
 * cSoftHdDevice state transitioning functions
 ********************************************************************/

/**
 * With this wrapper function, the device can directly act
 * as an event reveiver.
 *
 * @param event           event to be executed
 */
void cSoftHdDevice::TriggerEvent(const Event &event)
{
	m_pStateMachine->OnEventReceived(event);
}

/**
 * Actions to be performed when leaving a state
 *
 * These are only executed when the state actually changes.
 * E.g. a state transition PLAY -> PLAY does not trigger this.
 *
 * @param state         state being left
 */
void cSoftHdDevice::LeaveState(State state)
{
	switch (state) {
		case PLAY:
			m_pRender->SchedulePlaybackStartAtPtsMs(AV_NOPTS_VALUE);
			m_pRender->SetPlaybackPaused(true);
			m_pAudio->SetPaused(true);
			m_pAudio->ResetHwDelayBaseline();
			break;
		case BUFFERING:
			m_pAudio->SetHwDelayBaseline();
			m_pRender->SetDisplayOneFrameThenPause(false);
			break;
		case TRICK_SPEED:
			// The filter thread needs to be restarted for interlaced streams to be rendered with deinterlacer again. It is started lazily.
			m_pVideoStream->CancelFilterThread();
			m_pRender->SetTrickSpeed(0, false, false);
			m_pRender->ResetFrameCounter();
			m_pVideoStream->ResetFilterThreadNeededCheck();
			m_pVideoStream->SetDeinterlacerDeactivated(false);
			m_pRender->SetPlaybackPaused(true);
			m_pRender->ResetBufferReuseStrategy();
			m_pVideoStream->ResetTrickSpeedFramesSentCounter();
			break;
		case STOP:
			m_receivedAudio = false;
			m_receivedVideo = false;
			m_receivedValidAudio = false;
			m_receivedValidVideo = false;
			break;
		case DETACHED:
			m_pAudio = new cSoftHdAudio(this);
			m_pRender = new cVideoRender(this);
			m_pGrab = new cSoftHdGrab(m_pRender);
			m_pVideoStream = new cMainVideoStream(m_pRender, m_pHardwareDevice->GetQuirks(), m_pRender->GetMainOutputBuffer(), m_pConfig, std::bind(&cVideoRender::PushMainFrame, m_pRender, std::placeholders::_1));
			m_pAudioDecoder = new cAudioDecoder(m_pAudio);
			m_pRender->Init(); // starts display thread
			m_pVideoStream->StartDecoder(); // starts decoding thread
			m_pPipStream = new cPipVideoStream(m_pRender, m_pHardwareDevice->GetQuirks(), m_pRender->GetPipOutputBuffer(), m_pConfig, std::bind(&cVideoRender::PushPipFrame, m_pRender, std::placeholders::_1));
			m_pPipStream->StartDecoder(); // starts decoding thread
			m_pPipHandler = new cPipHandler(this);
			// Audio is init lazily (includes starting thread)

			break;
	}
}

/**
 * Actions to be performed when entering a state
 *
 * These are only executed when the state actually changes.
 * E.g. a state transition PLAY -> PLAY does not trigger this.
 *
 * @param state         state being entered
 */
void cSoftHdDevice::EnterState(State state)
{
	switch (state) {
		case BUFFERING:
			m_pAudio->ResetHwDelayBaseline();
			// nothing
			break;
		case PLAY:
			if (m_playbackMode != VIDEO_ONLY)
				m_pAudio->SetPaused(false);

			if (m_playbackMode != AUDIO_ONLY) {
				m_pVideoStream->SetDeinterlacerDeactivated(false);
				m_pRender->SetStillpicture(false);
				m_pRender->SetPlaybackPaused(false);
			}
			break;
		case TRICK_SPEED:
			// The filter thread needs to be restarted for interlaced streams to be rendered without deinterlacer in trick speed mode. It is started lazily.
			m_pVideoStream->CancelFilterThread();
			m_pRender->SetPlaybackPaused(false);
			m_pVideoStream->SetDeinterlacerDeactivated(true);
			m_pRender->ResetBufferReuseStrategy();
			break;
		case STOP:
			FlushAudio();

			m_pVideoStream->CancelFilterThread();
			m_pRender->DisplayBlackFrame();
			m_pRender->Reset();
			m_playbackMode = NONE;

			m_videoReassemblyBuffer.Reset();
			m_pVideoStream->ClearVdrCoreToDecoderQueue();
			m_pRender->ClearDecoderToDisplayQueue();
			m_pRender->ResetDecodingStrategy();
			m_pRender->ResetBufferReuseStrategy();
			m_pVideoStream->CloseDecoder();
			m_audioJitterTracker.Reset();
			m_videoJitterTracker.Reset();

			break;
		case DETACHED:
			delete m_pPipHandler;
			m_pPipHandler = nullptr;

			// resume the previously stopped threads
			m_pVideoStream->Resume();
			m_pRender->Resume();

			// now do the detach
			m_pPipStream->Exit();
			delete m_pPipStream;

#ifdef USE_GLES
			// The opengl thread was probably locked before cmdCopyBufferToOutputFb().
			// 1) set running to false
			// 2) continue the thread (which will skip the waiting cmd->Execute())
			// 3) do the real thread cancel and cleanup
			// We need to keep this order to prevent a deadlock!
			m_pOsdProvider->RequestStopOpenGlThread();
			m_pOsdProvider->UnlockOpenGlThread();
			m_pOsdProvider->StopOpenGlThread();
#endif
			m_pRender->Exit(); // render must be stopped before videostream!
			m_pVideoStream->Exit();
			m_pAudio->Exit(); // audio must be stopped after renderer!

			delete m_pAudioDecoder; // includes a Close()
			delete m_pVideoStream;
			delete m_pGrab;
			delete m_pRender;
			delete m_pAudio;

			break;
	}
}

/**
 * Pause the rendering and decoder thread.
 */
void cSoftHdDevice::HaltVideoThreads(void)
{
	m_pRender->Halt();
	m_pVideoStream->Halt();
}

/**
 * Resume the rendering and decoder thread.
 */
void cSoftHdDevice::ResumeVideoThreads(void)
{
	m_pVideoStream->Resume();
	m_pRender->Resume();
}

#ifdef USE_GLES
/**
 * Pause the OpenGL worker thread.
 */
bool cSoftHdDevice::HaltOpenGlThread(void)
{
	return m_pOsdProvider && m_pOsdProvider->LockOpenGlThread();
}

/**
 * Resume the OpenGL worker thread.
 */
void cSoftHdDevice::ResumeOpenGlThread(void)
{
	if (m_pOsdProvider)
		m_pOsdProvider->UnlockOpenGlThread();
}
#endif

/**
 * Returns true, the a detached plugin start was forced.
 */
bool cSoftHdDevice::IsDetachForced(void)
{
	return m_forceDetached;
}

/**
 * Init the audio lazily.
 *
 * @param setVolume           if true, reset the current volume
 */
void cSoftHdDevice::InitAudio(bool setVolume)
{
	m_pAudio->LazyInit();
	if (setVolume)
		m_pAudio->SetVolume((m_volume * 1000) / 255);
}

/**
 * Reset the video deinterlace filter
 */
void cSoftHdDevice::ResetVideoFilter(void)
{
	m_pRender->ResetFrameCounter();
	m_pVideoStream->ResetFilterThreadNeededCheck();
}

/**
 * Set the trickspeed in the renderer
 *
 * @param speed           trickspeed factor
 * @param active          true, if trickspeed is active
 * @param forward         true, if this is forward trickspeed
 */
void cSoftHdDevice::SetTrickSpeed(double speed, bool active, bool forward)
{
	m_pRender->SetTrickSpeed(speed, active, forward);
}

/**
 * Schedules the playback start
 *
 * Drops audio data if necessary to start in sync and schedules
 * the playback start at the common pts.
 *
 * @retval true                  playback started
 * @retval false                 playback was not started
 */
bool cSoftHdDevice::SchedulePlaybackStart(void)
{
	bool receivedAudio = m_pAudio->HasInputPts();
	bool receivedVideo = m_pVideoStream->HasInputPts();

	if (receivedAudio && receivedVideo) {
		m_playbackMode = AUDIO_AND_VIDEO;
		int64_t firstAudioPtsMs = GetFirstAudioPtsMsToPlay();
		int64_t firstVideoPtsMs = GetFirstVideoPtsMsToPlay();

		// store the first PTSes beforehand, because dropping samples/frames will change the output of GetFirst*PtsMsToPlay()
		m_pAudio->DropSamplesOlderThanPtsMs(firstAudioPtsMs);
		m_pRender->SchedulePlaybackStartAtPtsMs(firstVideoPtsMs);
	} else if (receivedAudio) {
		LOGDEBUG("device: audio only detected");
		m_playbackMode = AUDIO_ONLY;
		m_pAudio->DropSamplesOlderThanPtsMs(m_pAudio->GetOutputPtsMs());
	} else if (receivedVideo) {
		LOGDEBUG("device: video only detected");
		m_playbackMode = VIDEO_ONLY;
		m_pRender->SchedulePlaybackStartAtPtsMs(m_pRender->GetOutputPtsMs());
	} else {
		// Sometimes a DeviceClear() can jump in between signalling the ThresholdReachedEvent
		// and progressing it, e.g. the video thread signals the ThresholdReached, VDR sends a DeviceClear()
		// and OnEventReceived wants to process the ThresholdReachedEvent with empty buffers.
		LOGDEBUG("device: buffering threshold reached and no a/v available, keep BUFFERING state");
		return false;
	}

	return true;
}

/**
 * Schedule aa a/v resync in the render thread at the given pts
 *
 * @param pts            resync, when the rendered video frame reaches this pts
 */
void cSoftHdDevice::ScheduleResyncAtPtsMs(int64_t pts)
{
	m_pRender->ScheduleResyncAtPtsMs(pts);
}

/**
 * Resume playback from pause state
 *
 * Other than ResumePlayback() this also syncs a/v (by dropping audio)
 */
void cSoftHdDevice::ResumeFromPause(void)
{
	// resume from pause
	int audioBehindVideoByMs;

	switch (m_playbackMode) {
		case AUDIO_ONLY:
			m_pAudio->SetHwDelayBaseline();
			m_pAudio->SetPaused(false);
			break;
		case AUDIO_AND_VIDEO:
			audioBehindVideoByMs = m_pRender->GetOutputPtsMs() - m_pAudio->GetOutputPtsMs() - m_pConfig->ConfigVideoAudioDelayMs;
			m_pAudio->SetHwDelayBaseline();
			if (audioBehindVideoByMs > 0) {
				m_pAudio->DropSamplesOlderThanPtsMs(m_pAudio->GetOutputPtsMs() + audioBehindVideoByMs);
				m_pAudio->SetPaused(false);
			} else
				m_pRender->SetScheduleAudioResume(true);

			// fallthrough
		case VIDEO_ONLY:
			m_pRender->SetStillpicture(false);
			m_pRender->SetPlaybackPaused(false);
			break;
		case NONE:
			LOGFATAL("STATE MACHINE: play event in PLAY state with NONE playback mode. This is a bug.");
			break;
	}
}

/**
 * Pause the playback
 *
 * @param schedulePause               if true, pause at the current audio pts
 *                                    otherwise pause immediately
 */
void cSoftHdDevice::PausePlayback(bool schedulePause)
{
	if (schedulePause && m_playbackMode == AUDIO_AND_VIDEO)
		m_pRender->ScheduleVideoPlaybackPauseAt(m_pAudio->GetOutputPtsMs() - m_pConfig->ConfigVideoAudioDelayMs);
	else
		m_pRender->SetPlaybackPaused(true);

	m_pAudio->SetPaused(true);
	m_pAudio->ResetHwDelayBaseline();
}

/**
 * Resume playback again
 */
void cSoftHdDevice::ResumePlayback(void)
{
	m_pRender->SetPlaybackPaused(false);
}
