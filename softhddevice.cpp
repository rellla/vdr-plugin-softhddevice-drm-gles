/**
 * @file softhddevice.cpp
 * Device class
 *
 * This file defines cSoftHdDevice which is the implementation
 * of cDevice. This is the place where all the device commands
 * which are sent be VDR are placed in (i.e. Play(), TrickSpeed() ...)
 *
 * @copyright (c) 2011 - 2015 by Johns.  All Rights Reserved.
 * @copyright (c) 2018 - 2019 by zille.  All Rights Reserved.
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

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <mutex>
#include <variant>

#include <assert.h>
#include <unistd.h>

#include <libintl.h>

#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "softhddevice-drm-gles.h"
#include "softhdosd.h"

#include "softhddevice.h"
#include "logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>

}
#include "buf2rgb.h"

#include "iatomic.h"
#include "videostream.h"
#include "audio.h"
#include "videorender.h"
#include "codec_audio.h"
#include "codec_video.h"
#include "pes.h"
#include "misc.h"
#include "pipreceiver.h"

#define _(str) gettext(str)                    ///< gettext shortcut
#define _N(str) str                            ///< gettext_noop shortcut
#define AUDIO_MIN_BUFFER_FREE (3072 * 8 * 8)   ///< Minimum free space in audio buffer 8 packets for 8 channels
#define AUDIO_BUFFER_SIZE (512 * 1024)         ///< audio PES buffer default size

/**
 * Call rgb to jpeg for C Plugin
 */
extern "C" uint8_t * CreateJpeg(uint8_t * image, int *size, int quality,
	int width, int height)
{
	return (uint8_t *) RgbToJpeg((uchar *) image, width, height, *size, quality);
}

#if defined(USE_JPEG) && JPEG_LIB_VERSION >= 80
/**
 * Create a jpeg image in memory
 *
 * @param image      raw RGB image
 * @param raw_size   size of raw image
 * @param size[out]  size of jpeg image
 * @param quality    jpeg quality
 * @param width      number of horizontal pixels in image
 * @param height     number of vertical pixels in image
 *
 * @returns allocated jpeg image.
 */
uint8_t *CreateJpeg(uint8_t * image, int raw_size, int *size, int quality,
	int width, int height)
{
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	JSAMPROW row_ptr[1];
	int rowStride;
	uint8_t *outbuf;
	long unsigned int outsize;

	outbuf = NULL;
	outsize = 0;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	jpeg_mem_dest(&cinfo, &outbuf, &outsize);

	cinfo.image_width = width;
	cinfo.image_height = height;
	cinfo.input_components = raw_size / height / width;
	cinfo.in_color_space = JCS_RGB;

	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);

	rowStride = width * 3;
	while (cinfo.next_scanline < cinfo.image_height) {
		row_ptr[0] = &image[cinfo.next_scanline * rowStride];
		jpeg_write_scanlines(&cinfo, row_ptr, 1);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	*size = outsize;

	return outbuf;
}
#endif

/*****************************************************************************
 * cSoftHdDevice class
 ****************************************************************************/

/**
 * cSoftHdDevice constructor
 *
 * Initializes some member variables
 *
 * @param config       pointer to cSoftHdConfig class
 */
cSoftHdDevice::cSoftHdDevice(cSoftHdConfig *config)
{
//	LOGDEBUG("device: %s:", __FUNCTION__);

	m_pSpuDecoder = new cDvbSpuDecoder();
	m_pConfig = config;
	m_pAudioDecoder = nullptr;
	m_videoAudioDelay = m_pConfig->ConfigVideoAudioDelay;
	m_audioChannelID = -1;
	m_pOsdProvider = nullptr;
	m_pipActive = false;
}

/**
 * cSoftHdDevice destructor
 *
 * only deletes spu decoder, which was created in constructor
 */
cSoftHdDevice::~cSoftHdDevice(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	delete m_pSpuDecoder;
}

/**
 * Device init
 *
 * .. currently does nothing
 */
void cSoftHdDevice::Init(void)
{
	LOGDEBUG("device: %s: (do nothing)", __FUNCTION__);
}

/**
 * Start plugin
 *
 * creates
 *     audio device
 *     render device
 *     video stream device
 *     audio decoder
 *
 * inits
 *     renderer
 *
 * starts
 *     display thread (filter thread starts on demand)
 *     decoding thread
 *
 * No need to init audio and start audio thread, because this is done lazily.
 */
void cSoftHdDevice::Start(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);

	OnEventReceived(AttachEvent{});
}

/**
 * Stop plugin
 *
 * Stop and delete everything except the config and device itself
 *
 */
void cSoftHdDevice::Stop(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);

	OnEventReceived(DetachEvent{});
}

/**
 * Clear all audio data from the decoder and ringbuffer
 */
void cSoftHdDevice::ClearAudio(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	m_pAudioDecoder->FlushBuffers();
	m_pAudio->FlushBuffers();
	m_audioReassemblyBuffer.Reset();
}

/**
 * Informs a device that it will be the primary device
 *
 * @param on	flag if becoming or loosing primary
 */
void cSoftHdDevice::MakePrimaryDevice(bool on)
{
	LOGDEBUG("device: %s: %d", __FUNCTION__, on);
	if (!on)
		Stop();
	else
		Start();

	cDevice::MakePrimaryDevice(on);
	if (on)
		m_pOsdProvider = new cSoftOsdProvider(this); // no need to delete it, VDR does it
}

/**
 * Get the device SPU decoder.
 *
 * @returns a pointer to the device's SPU decoder
 *          (or NULL, if thisdevice doesn't have an SPU decoder)
 */
cSpuDecoder *cSoftHdDevice::GetSpuDecoder(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	if (!IsPrimaryDevice())
		return NULL;

	return m_pSpuDecoder;
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
 * Returns true if this device can currently start a replay session
 */
bool cSoftHdDevice::CanReplay(void) const
{
	bool canReplay = !IsDetached();

	LOGDEBUG("device: %s: %d", __FUNCTION__, canReplay);

	return canReplay;
}

/**
 * Handle pause state
 *
 * Pauses both video rendering and audio playback.
 */
void cSoftHdDevice::HandlePause(void)
{
	m_pRender->SetPlaybackPaused(true);
	m_pAudio->Pause();
}

/**
 * Event handler for playback state transitions
 *
 * Processes events (Play, Pause, Stop, TrickSpeed, StillPicture) and performs
 * the appropriate state transitions based on the current state. The method halts
 * both display and decoding threads before processing the event and resumes them
 * afterwards to ensure thread-safe state transitions.
 *
 * @param event     The event to process (variant type containing specific event data)
 */
void cSoftHdDevice::OnEventReceived(const Event& event)
{
	uint64_t startStateChange = cTimeMs::Now();
	std::lock_guard<std::mutex> lock(m_mutex);

	LOGDEBUG("device: received %s", EventToString(event));

	if (m_state != DETACHED) {
		m_pRender->DisplayThreadHalt(); // the display thread needs to be halted first, otherwise a deadlock can occur in WaitForAudioClock()
		m_pVideoStream->DecodingThreadHalt();
	}

	bool needsResume = true;

	auto invalid = [this, &event]() {
		LOGWARNING("device: Invalid event '%s' in state '%s' received", EventToString(event), StateToString(m_state));
	};

	switch (m_state) {
		case State::DETACHED:
			std::visit(overload{
				[&invalid](const PlayEvent&) { invalid(); },
				[&invalid](const PauseEvent&) { invalid(); },
				[&invalid](const StopEvent&) { invalid(); },
				[&invalid](const TrickSpeedEvent&) { invalid(); },
				[&invalid](const StillPictureEvent&) { invalid(); },
				[&invalid](const DetachEvent&) { invalid(); },
				[this](const AttachEvent&) {
					SetState(STOP);
				},
				[&invalid](const PipStartEvent&) { invalid(); },
				[&invalid](const PipStopEvent&) { invalid(); },
			}, event);
			needsResume = false;
			break;
		case State::STOP:
			std::visit(overload{
				[this](const PlayEvent&) {
					m_pAudio->LazyInit();
					SetState(PLAY);
					m_pRender->ResetFrameCounter();
				},
				[&invalid](const PauseEvent&) { invalid(); },
				[&invalid](const StopEvent&) { invalid(); },
				[&invalid](const TrickSpeedEvent&) { invalid(); },
				[&invalid](const StillPictureEvent&) { invalid(); },
				[this, &needsResume](const DetachEvent&) {
					SetEnablePip(false);
					SetState(DETACHED);
					needsResume = false;
				},
				[&invalid](const AttachEvent&) { invalid(); },
				[this](const PipStartEvent&) {
					SetEnablePip(true);
				},
				[this](const PipStopEvent&) {
					SetEnablePip(false);
				},
			}, event);
			break;
		case State::PLAY:
			std::visit(overload{
				[this](const PlayEvent&) {
					// resume from pause
					m_pAudio->Resume();
					m_pRender->SetPlaybackPaused(false);
				},
				[this](const PauseEvent&) {
					HandlePause();
				 },
				[this](const StopEvent&) {
					SetState(STOP);
				},
				[this](const TrickSpeedEvent& t) {
					m_pRender->SetTrickSpeed(t.speed, t.forward);
					SetState(TRICK_SPEED);
				},
				[this](const StillPictureEvent& s) {
					HandleStillPicture(s.data, s.size);
				 },
				[this, &needsResume](const DetachEvent&) {
					SetEnablePip(false);
					SetState(DETACHED);
					needsResume = false;
				},
				[&invalid](const AttachEvent&) { invalid(); },
				[this](const PipStartEvent&) {
					SetEnablePip(true);
				},
				[this](const PipStopEvent&) {
					SetEnablePip(false);
				},
			}, event);
			break;
		case State::TRICK_SPEED:
			std::visit(overload{
				[this](const PlayEvent&) {
					SetState(PLAY);
				},
				[this](const PauseEvent&) {
					HandlePause();
				 },
				[this](const StopEvent&) {
					SetState(STOP);
				},
				[this](const TrickSpeedEvent& t) {
					// resume from pause, or change trick speed direction/speed
					m_pRender->SetTrickSpeed(t.speed, t.forward);
					m_pRender->SetPlaybackPaused(false);
				 },
				[this](const StillPictureEvent& s) {
					HandleStillPicture(s.data, s.size);
				 },
				[this, &needsResume](const DetachEvent&) {
					SetEnablePip(false);
					SetState(DETACHED);
					needsResume = false;
				},
				[&invalid](const AttachEvent&) { invalid(); },
				[this](const PipStartEvent&) {
					SetEnablePip(true);
				},
				[this](const PipStopEvent&) {
					SetEnablePip(false);
				},
			}, event);
			break;
		case State::STILL_PICTURE:
			std::visit(overload{
				[this](const PlayEvent&) {
					SetState(PLAY);
				},
				[&invalid](const PauseEvent&) { invalid(); },
				[this](const StopEvent&) {
					SetState(STOP);
				},
				[this](const TrickSpeedEvent& t) {
					m_pRender->SetTrickSpeed(t.speed, t.forward);
					SetState(TRICK_SPEED);
				},
				[this](const StillPictureEvent& s) {
					HandleStillPicture(s.data, s.size);
				 },
				[this, &needsResume](const DetachEvent&) {
					SetEnablePip(false);
					SetState(DETACHED);
					needsResume = false;
				},
				[&invalid](const AttachEvent&) { invalid(); },
				[this](const PipStartEvent&) {
					SetEnablePip(true);
				},
				[this](const PipStopEvent&) {
					SetEnablePip(false);
				},
			}, event);
			break;
	}

	if (needsResume) {
		m_pVideoStream->DecodingThreadResume();
		m_pRender->DisplayThreadResume();
	}

	uint64_t stopStateChange = cTimeMs::Now();
	LOGDEBUG("device: state change done in %d ms", (int)(stopStateChange - startStateChange));
}

/**
 * Actions to be performed when entering a state
 *
 * These are only executed when the state actually changes.
 * E.g. a state transition PLAY -> PLAY does not trigger this.
 *
 * @param state         state being entered
 */
void cSoftHdDevice::OnEnteringState(enum State state) {
	switch (state) {
		case PLAY:
			m_pAudio->Resume();
			m_pRender->SetPlaybackPaused(false);
			break;
		case TRICK_SPEED:
			// The filter thread needs to be restarted for interlaced streams to be rendered without deinterlacer in trick speed mode. It is started lazily.
			m_pRender->CancelFilterThread();
			m_pRender->SetPlaybackPaused(false);
			m_pRender->SetDeinterlacerDeactivated(true);
			m_pAudio->Pause();
			break;
		case STOP:
			m_pRender->CancelFilterThread();

			m_pRender->Reset();
			m_pRender->DestroyFrameBuffers();
			m_pRender->ScheduleDisplayBlackFrame();
			m_videoReassemblyBuffer.Reset();
			m_pVideoStream->ClearVdrCoreToDecoderQueue();
			m_pRender->ClearDecoderToDisplayQueue();
			m_pVideoStream->CloseDecoder();

			m_pAudio->Resume();
			ClearAudio();
			break;
		case STILL_PICTURE:
			m_pRender->SetDeinterlacerDeactivated(true);
			break;
		case DETACHED:
			// do the same cleanup as in STOP first except audio resume and flushing
			m_pRender->CancelFilterThread();

			m_pRender->Reset();
			m_pRender->DestroyFrameBuffers();
			m_pRender->ScheduleDisplayBlackFrame();

			m_videoReassemblyBuffer.Reset();
			m_pVideoStream->ClearVdrCoreToDecoderQueue();
			m_pRender->ClearDecoderToDisplayQueue();
			m_pVideoStream->CloseDecoder();

			m_audioReassemblyBuffer.Reset();

			// resume the previously stopped threads
			m_pVideoStream->DecodingThreadResume();
			m_pRender->DisplayThreadResume();

			// now do the detach
			m_pPipStream->Exit();
			delete m_pPipStream;

			m_pAudio->Exit();
			m_pRender->Exit(); // render must be stopped before videostream!
			m_pVideoStream->Exit();
#ifdef USE_GLES
			m_pOsdProvider->StopOpenGlThread();
#endif
			delete m_pAudioDecoder; // includes a Close()
			delete m_pVideoStream;
			delete m_pRender;
			delete m_pAudio;
			break;
	}
}

/**
 * Actions to be performed when leaving a state
 *
 * These are only executed when the state actually changes.
 * E.g. a state transition PLAY -> PLAY does not trigger this.
 *
 * @param state         state being left
 */
void cSoftHdDevice::OnLeavingState(enum State state) {
	switch (state) {
		case PLAY:
			// nothing
			break;
		case TRICK_SPEED:
			// The filter thread needs to be restarted for interlaced streams to be rendered with deinterlacer again. It is started lazily.
			m_pRender->CancelFilterThread();

			m_pRender->DestroyFrameBuffers();

			m_pRender->SetTrickSpeed(0, 1);
			m_pRender->ResetFrameCounter();
			m_pRender->SetDeinterlacerDeactivated(false);
			m_pVideoStream->ResetTrickSpeedFramesSentCounter();

			m_pAudio->Resume();
			break;
		case STOP:
			// nothing
			break;
		case STILL_PICTURE:
			m_pRender->SetDeinterlacerDeactivated(false);
			break;
		case DETACHED:
			m_pAudio = new cSoftHdAudio(this);
			m_pAudio->LazyInit();
			m_pRender = new cVideoRender(this);
			m_pVideoStream = new cMainVideoStream(this);
			m_pAudioDecoder = new cAudioDecoder(m_pAudio);
			m_pRender->Init(); // starts display thread
			m_pVideoStream->StartDecoder(new cVideoDecoder(m_pRender->HardwareQuirks()), "shd vid decode"); // starts decoding thread
			m_pPipStream = new cPipVideoStream(this);
			m_pPipStream->StartDecoder(new cVideoDecoder(m_pRender->HardwareQuirks()), "shd pip decode"); // starts decoding thread
			// Audio is init lazily (includes starting thread)
			break;
	}
}


/**
 * Sets the device into the given state.
 *
 * @param newState       new state
 */
void cSoftHdDevice::SetState(enum State newState)
{
	// No synchronization needed, because SetState is only called from the VDR main thread.

	if (m_state != newState) {
		LOGDEBUG("device: Preparing to leave state %s", StateToString(m_state));
		OnLeavingState(m_state);
		LOGDEBUG("device: Changing state %s -> %s", StateToString(m_state), StateToString(newState));
		m_state = newState;
		OnEnteringState(m_state);
		LOGDEBUG("device: State changed to %s", StateToString(m_state));
	}
}

/**
 * Sets the device into the given play mode.
 *
 * @param play_mode       new play mode (Audio/Video/External...)
 */
bool cSoftHdDevice::SetPlayMode(ePlayMode play_mode)
{
	LOGDEBUG("device: %s: %d", __FUNCTION__, play_mode);

	switch (play_mode) {
	case pmNone:
		OnEventReceived(StopEvent{});
		break;
	case pmAudioVideo:
	case pmAudioOnly:
	case pmAudioOnlyBlack:
	case pmVideoOnly:
		OnEventReceived(PlayEvent{});
		break;
	default:
		LOGERROR("device: %s: playmode not supported %d", play_mode);
		return 0;
		break;
	}

	return 1;
}

/**
 * Gets the current System Time Counter, which can be used to
 *        synchronize audio, video and subtitles.
 */
int64_t cSoftHdDevice::GetSTC(void)
{
//    LOGDEBUG("%s:", __FUNCTION__);
	if (IsDetached())
		return AV_NOPTS_VALUE;

	if (m_pRender)
		return m_pRender->GetVideoClock();

	// could happen during dettached
	LOGWARNING("device: %s: called without hw decoder", __FUNCTION__);
	return AV_NOPTS_VALUE;
}

/**
 * Set trick play speed.
 *
 * Every single frame shall then be displayed the given number of
 * times.
 *
 * @param speed       trick speed
 * @param forward     flag forward direction
 */
void cSoftHdDevice::TrickSpeed(int speed, bool forward)
{
	LOGDEBUG("device: %s: %d %s", __FUNCTION__, speed, forward ? "forward" : "backward");

	OnEventReceived(TrickSpeedEvent{speed, forward});
}

/**
 * Clears all video and audio data from the device.
 *
 * This is called by VDR via DeviceClear() in the Empty() call.
 *
 * Empty() does clear all VDR internal packets.
 *
 */
void cSoftHdDevice::Clear(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	cDevice::Clear();

	if (IsDetached())
		return;

	m_pRender->DisplayThreadHalt(); // the display thread needs to be halted first, otherwise a deadlock can occur in WaitForAudioClock()
	m_pVideoStream->DecodingThreadHalt();

	m_pRender->CancelFilterThread();

	m_videoReassemblyBuffer.Reset();
	m_pVideoStream->ClearVdrCoreToDecoderQueue();
	m_pRender->ClearDecoderToDisplayQueue();

	if (m_pVideoStream->GetCodecId() != AV_CODEC_ID_NONE) // audio only (e.g. radio) has no video codec. So, don't flush the video decoder then.
		m_pVideoStream->FlushDecoder();

	m_pRender->DestroyFrameBuffers();
	m_pRender->Reset();

	ClearAudio();

	m_pRender->DisplayThreadResume();
	m_pVideoStream->DecodingThreadResume();
}

/**
 * Sets the device into play mode (after a previous trick mode, or pause)
 *
 * This is called by VDR via DevicePlay() in the Play() and Goto() call
 *
 */
void cSoftHdDevice::Play(void)
{
	cDevice::Play();

	OnEventReceived(PlayEvent{});
}

/**
 * Puts the device into "freeze frame" mode.
 */
void cSoftHdDevice::Freeze(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	cDevice::Freeze();

	OnEventReceived(PauseEvent{});
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

	OnEventReceived(StillPictureEvent{data, size});
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
	SetState(STILL_PICTURE);

	const uchar *currentPacketStart = data;
	while (currentPacketStart < data + size) {
		cPesVideo pesPacket((const uint8_t*)currentPacketStart, size - (currentPacketStart - data));

		if (pesPacket.IsValid())
			PlayVideoInternal(m_pVideoStream, &m_videoReassemblyBuffer, currentPacketStart, pesPacket.GetPacketLength());
		else {
			LOGWARNING("device: %s: invalid PES packet", __FUNCTION__);
			break;
		}

		currentPacketStart += pesPacket.GetPacketLength();
	}

	m_pVideoStream->PushAvPacket(m_videoReassemblyBuffer.PopAvPacket());
	m_pVideoStream->Flush();
}

/**
 * Check if the device is ready for further action.
 *
 * This function is useless, the return value is ignored and
 * all buffers are overrun by vdr.
 *
 * The dvd plugin is using this correct.
 *
 * @param poller        file handles (unused)
 * @param timeout_ms    timeout in ms to become ready
 *
 * @retval true         if ready
 * @retval false        if busy
 */
bool cSoftHdDevice::Poll(__attribute__ ((unused)) cPoller & poller, int timeout)
{
//	LOGDEBUG("device: %s: timeout %d", __FUNCTION__, timeout_ms);

	if (IsDetached())
		return true;

	for (;;) {
		int full;
		int t;
		int used;
		int filled;

//		LOGDEBUG("device: %s: timeout %d", __FUNCTION__, timeout);

		used = m_pAudio->GetUsedBytes();
		// FIXME: no video!
		filled = m_pVideoStream->GetAvPacketsFilled();
		// soft limit + hard limit
		full = (used > AUDIO_MIN_BUFFER_FREE && filled > 3) ||
		        m_pAudio->GetFreeBytes() < AUDIO_MIN_BUFFER_FREE ||
		        filled >= VIDEO_PACKET_MAX - 10;

		if (!full || !timeout) {
			return !full;
		}

		t = 15;
		if (timeout < t) {
			t = timeout;
		}
		usleep(t * 1000);		// let display thread work
		timeout -= t;
	}

	return true;
}

/**
 * Flush the device output buffers.
 *
 * @param timeout_ms        timeout in ms to become ready
 */
bool cSoftHdDevice::Flush(int timeout)
{
	if (IsDetached())
		return true;

	LOGDEBUG("device: %s: timeout %d ms", __FUNCTION__, timeout);
	if (m_pVideoStream->GetAvPacketsFilled()) {
		if (timeout) {			// let display thread work
			usleep(timeout * 1000);
		}
		return !m_pVideoStream->GetAvPacketsFilled();
	}

	return true;
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
 * Returns the width, height and aspect ratio of the currently
 * displayed video material
 *
 * @note the video_aspect is used to scale the subtitle.
 */
void cSoftHdDevice::GetVideoSize(int &width, int &height, double &aspectRatio)
{
//	LOGDEBUG("device: %s: %d x %d @ %f", __FUNCTION__, *width, *height, *aspectRatio);
	if (IsDetached())
		return;

	m_pVideoStream->GetVideoSize(&width, &height, &aspectRatio);
}

/**
 * Returns the width, height and aspect ratio the OSD
 *
 * FIXME: Called every second, for nothing (no OSD displayed)?
 */
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &aspectRatio)
{
	if (IsDetached())
		return;

	std::lock_guard<std::mutex> lock(m_sizeMutex);
	width = m_screenWidth;
	height = m_screenHeight;
	aspectRatio = (double)width / (double)height;
}

/**
 * Set the screen size
 *
 * @param width           screen width
 * @param height          screen height
 * @param refreshRate     screen refresh rate (currently unused)
 */
void cSoftHdDevice::SetScreenSize(int width, int height, uint32_t refreshRate)
{
	std::lock_guard<std::mutex> lock(m_sizeMutex);
	m_screenWidth = width;
	m_screenHeight = height;
	m_screenRefreshRate = refreshRate;
}

/**
 * Print the start code, stream id, length, first three bytes (start code) of the payload, and the following 16 bytes of the codec payload.
 *
 * @param data        pointer to stream data
 * @param offset      print from here
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
 * Play an audio packet
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

	// hard limit buffer full: don't overrun audio buffers on replay
	if (m_pAudio->GetFreeBytes() < AUDIO_MIN_BUFFER_FREE) {
//		LOGDEBUG("device: %s: Buffer is Full (%d|%d)!", __FUNCTION__, m_pAudio->GetFreeBytes(), AUDIO_MIN_BUFFER_FREE);
		return 0;
	}

	cPesAudio pesPacket((const uint8_t*)data, size);

	if (!pesPacket.IsValid()) {
		m_audioReassemblyBuffer.Reset();

		return size;
	}

	if (m_audioChannelID != id) {
		m_audioChannelID = id;
		m_audioReassemblyBuffer.Reset();
		m_pAudioDecoder->Close();
		LOGDEBUG("device: %s: new channel id 0x%02X", __FUNCTION__, m_audioChannelID);
	}

	m_audioReassemblyBuffer.Push(pesPacket.GetPayload(), pesPacket.GetPayloadSize(), pesPacket.GetPts());

	AVPacket *avpkt;
	do {
		avpkt = m_audioReassemblyBuffer.PopAvPacket();

		if (avpkt) {
			if (m_pAudioDecoder->GetCodecId() == AV_CODEC_ID_NONE && m_audioReassemblyBuffer.GetCodec() != AV_CODEC_ID_NONE) {
				// The playback has just started
				m_pAudioDecoder->Close();
				m_pAudioDecoder->Open(m_audioReassemblyBuffer.GetCodec());
			}

			m_pAudioDecoder->Decode(avpkt);
			AVPacket *copy = avpkt;
			av_packet_free(&copy);
		}
	} while (avpkt != nullptr);

	return size;
}

void cSoftHdDevice::SetAudioTrackDevice( __attribute__ ((unused)) eTrackType type)
{
	//LOGDEBUG("device: %s:", __FUNCTION__);
}

void cSoftHdDevice::SetDigitalAudioDevice( __attribute__ ((unused)) bool on)
{
	//LOGDEBUG("device: %s: %s", __FUNCTION__, on ? "true" : "false");
}

void cSoftHdDevice::SetAudioChannelDevice( __attribute__ ((unused))
	int audio_channel)
{
	//LOGDEBUG("device: %s: %d", __FUNCTION__, audio_channel);
}

int cSoftHdDevice::GetAudioChannelDevice(void)
{
	//LOGDEBUG("device: %s:", __FUNCTION__);
	return 0;
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
	m_pAudio->SetVolume((volume * 1000) / 255);
}

/**
 * Play a video packet of the main videostream
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
	return PlayVideoInternal(m_pVideoStream, &m_videoReassemblyBuffer, data, size);
}

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
	return PlayVideoInternal(m_pPipStream, &m_pipReassemblyBuffer, data, size);
}

/**
 * Play a video packet
 *
 * @param data    A complete PES packet with optionally fragmented payload
 * @param size    the length of the PES packet including header
 */
int cSoftHdDevice::PlayVideoInternal(cVideoStream *stream, cReassemblyBufferVideo *buffer, const uchar *data, int size)
{
	// LOGDEBUG("device: %s: %p %d", __FUNCTION__, data, size);

	if (stream->GetAvPacketsFilled() >= VIDEO_PACKET_MAX - 10)
		return 0;

	cPesVideo pesPacket((const uint8_t*)data, size);

	if (!pesPacket.IsValid()) {
		buffer->Reset();

		return size;
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
 * Grabs the currently visible screen image
 *
 * @param size      size of the returned data
 * @param jpeg      flag true, create JPEG data
 * @param quality   JPEG quality
 * @param width     number of horizontal pixels in the frame
 * @param height    number of vertical pixels in the frame
 *
 * This works as follows:
 *    1. Trigger the grab in render thread which clones the buffers
 *       Because of the cloning, the render thread is just blocked until the data is copied
 *    2. Convert these buffers to rgb and free the cloned buffers afterwards
 *    3. Get video and osd data
 *    4. Blit the video into a full black screen if it is scaled
 *    5. Blend the osd over video
 *    6. Scale the result to the requested size
 *    7. Create the jpeg or pnm
 */
uchar *cSoftHdDevice::GrabImage(int &size, bool jpeg, int quality, int width, int height)
{
	if (IsDetached())
		return NULL;

	if (m_grabActive) {
		LOGWARNING("device: %s: wait for the last grab to be finished - skip!", __FUNCTION__);
		return NULL;
	}

	if (!width || !height) {
		LOGERROR("device: %s: Width or height must be not 0!", __FUNCTION__);
		return NULL;
	}

	if (quality < 0) {	// caller should care, but fix it
		quality = 95;
	}

	LOGDEBUG2(L_GRAB, "device: %s: %d, %d, %d, %dx%d", __FUNCTION__, size, jpeg, quality, width, height);

	// 1. Trigger grab in render thread and wait for the buffers to be cloned
	m_grabActive = true;
	// TriggerGrab does wait and return 0, if buffers are available,
	// otherwise it returns != 0, if we ran into a timeout
	if (m_pRender->TriggerGrab()) {
		m_pRender->ClearGrab();
		m_grabActive = false;
		return NULL;
	}

	// 2. Convert the buffers to rgb and free the cloned buffers afterwards
	m_pRender->ConvertOsdBufToRgb();
	m_pRender->ConvertVideoBufToRgb();
	m_pRender->ConvertPipBufToRgb();

	// 3. get screen dimensions
	int screenWidth = 0;
	int screenHeight = 0;
	double aspectRatio = 0.0f;
	GetOsdSize(screenWidth, screenHeight, aspectRatio);

	int screenSize = screenWidth * screenHeight * 3; // we want a RGB24

	// 4. set grab dimensions
	int grabWidth = width > 0 ? width : screenWidth;
	int grabHeight = height > 0 ? height : screenHeight;

	int videoSize = 0;                // data size of the grabbed video
	int videoWidth = screenWidth;     // width of the grabbed video
	int videoHeight = screenHeight;   // height of the grabbed video
	int videoX = 0, videoY = 0;      // x, y of the grabbed video

	// 5. fetch video data
	// Video comes as RGB, width and height is original screen dimension (video is maybe scaled)
	cSoftHdGrab *videoGrab = m_pRender->GetGrab(&videoSize, &videoWidth, &videoHeight, &videoX, &videoY, 0);
	uint8_t *video = NULL;
	if (videoGrab->GetSize())
		video = videoGrab->GetData();
	if (!video) {
		LOGDEBUG2(L_GRAB, "device: %s: video is NULL, create black screen!", __FUNCTION__);
		video = (uint8_t *)calloc(1, screenSize);
	}

	int pipSize = 0;                // data size of the grabbed pip video
	int pipWidth = screenWidth;     // width of the grabbed pip video
	int pipHeight = screenHeight;   // height of the grabbed pip video
	int pipX = 0, pipY = 0;        // x, y of the grabbed pip video

	// 6. fetch pip data
	// Pip video comes as RGB, width and height is original screen dimension (video is maybe scaled)
	cSoftHdGrab *pipGrab = m_pRender->GetGrab(&pipSize, &pipWidth, &pipHeight, &pipX, &pipY, 2);
	uint8_t *pip = NULL;
	if (pipGrab->GetSize())
		pip = pipGrab->GetData();
	if (!pip)
		LOGDEBUG2(L_GRAB, "device: %s: pip is NULL, skip it", __FUNCTION__);

	// 7. fetch osd data
	// OSD comes as ARGB, width and height is original screen dimension (osd is always fullscreen)
	cSoftHdGrab *osdGrab = m_pRender->GetGrab(NULL, NULL, NULL, NULL, NULL, 1);
	uint8_t *osd = NULL;
	if (osdGrab->GetSize())
		osd = osdGrab->GetData();;
	if (!osd)
		LOGDEBUG2(L_GRAB, "device: %s: osd is NULL, skip it", __FUNCTION__);

	int ret;
	uint8_t *videoResult = NULL;
	// 8. blit the video into a full black screen if scaled
	if (videoWidth != screenWidth || videoHeight != screenHeight || videoX != 0 || videoY != 0) {
		videoResult = (uint8_t *)calloc(1, screenSize);
		ret = BlitVideo(videoResult, video, screenWidth, screenHeight, videoX, videoY, videoWidth, videoHeight);
		if (ret) {
			free(videoResult);
			free(video);
			return NULL;
		}
		free(video);
	} else {
		videoResult = video;
	}

	// 9. blit the pip video into the main video if available
	if (pip) {
		ret = BlitVideo(videoResult, pip, screenWidth, screenHeight, pipX, pipY, pipWidth, pipHeight);
		if (ret) {
			free(videoResult);
			free(pip);
			return NULL;
		}
		free(pip);
	}

	// 10. alphablend fullscreen video with osd if available
	uint8_t *result;
	if (!osd) {
		result = videoResult;
	} else {
		result = (uint8_t *)malloc(screenSize);
		AlphaBlend(result, osd, videoResult, screenWidth, screenHeight);
		free(videoResult);
		free(osd);
	}

	// 11. scale result to requested size width + height, if it differs from fullscreen
	int scaledSize = screenSize;
	uint8_t *scaledResult;
	if (screenWidth != grabWidth || screenHeight != grabHeight) {
		scaledResult = ScaleRgb24(result, &scaledSize, screenWidth, screenHeight, grabWidth, grabHeight);
		free(result);
	} else {
		scaledResult = result;
	}

	// 12. make jpeg or pnm
	uint8_t *grabbedImage;
	if (jpeg) {
		grabbedImage = CreateJpeg(scaledResult, &size, quality, grabWidth, grabHeight);
	} else {  // add header to raw data
		char buf[64];
		int n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n", grabWidth, grabHeight);
		grabbedImage = (uint8_t *)malloc(scaledSize + n);
		memcpy(grabbedImage, buf, n);
		memcpy(grabbedImage + n, scaledResult, scaledSize);
		size = scaledSize + n;
	}
	free(scaledResult);
	LOGDEBUG2(L_GRAB, "device: %s: finished %s image (%dx%d, quality %d) at %p (size %d)", __FUNCTION__, jpeg ? "jpg" : "pnm", grabWidth, grabHeight, jpeg ? quality : 0, grabbedImage, size);

	m_grabActive = false;
	return grabbedImage;
}

/**
 * Ask the output, if it can scale video
 *
 * @param rect      requested video window rectangle
 *
 * @returns         the real rectangle or cRect::NULL if invalid
 */
cRect cSoftHdDevice::CanScaleVideo(const cRect & rect, __attribute__ ((unused)) int alignment)
{
	return rect;
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
 * Return command line help string.
 */
const char *cSoftHdDevice::CommandLineHelp(void)
{
	return "  -a device\taudio device (fe. alsa: hw:0,0)\n"
	       "  -p device\taudio device for pass-through (hw:0,1)\n"
	       "  -c channel\taudio mixer channel name (fe. PCM)\n"
	       "  -d resolution\tdisplay resolution (fe. 1920x1080@50)\n"
#ifdef USE_GLES
	       "  -w workaround\tenable/disable workarounds\n"
	       "\tdisable-ogl-osd disable openGL osd\n"
#endif
	       "\n";
}

/**
 * Process the command line arguments.
 *
 * @param argc	number of arguments
 * @param argv	arguments vector
 */
int cSoftHdDevice::ProcessArgs(int argc, char *argv[])
{
	//
	//	Parse arguments.
	//

	for (;;) {
#ifdef USE_GLES
		switch (getopt(argc, argv, "-a:c:p:d:w:")) {
#else
		switch (getopt(argc, argv, "-a:c:p:d:")) {
#endif
		case 'a':           // audio device for pcm
			m_pConfig->ConfigAudioPCMDevice = optarg;
			continue;
		case 'c':           // channel of audio mixer
			m_pConfig->ConfigAudioMixerChannel = optarg;
			continue;
		case 'p':           // pass-through audio device
			m_pConfig->ConfigAudioPassthroughDevice = optarg;
			continue;
		case 'd':           // set display output
			m_pConfig->ConfigDisplayResolution = optarg;
			continue;
#ifdef USE_GLES
		case 'w':           // workarounds
			if (!strcasecmp("disable-ogl-osd", optarg)) {
				SetDisableOglOsd();
			} else {
				fprintf(stderr, _("Workaround '%s' unsupported\n"),
				optarg);
				return 0;
			}
			continue;
#endif
		case EOF:
			break;
		case '-':
			fprintf(stderr, _("We need no long options\n"));
			return 0;
		case ':':
			fprintf(stderr, _("Missing argument for option '%c'\n"), optopt);
			return 0;
		default:
			fprintf(stderr, _("Unknown option '%c'\n"), optopt);
			return 0;
		}
		break;
	}

	while (optind < argc) {
		fprintf(stderr, _("Unhandled argument '%s'\n"), argv[optind++]);
	}

	return 1;
}

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

#ifdef USE_GLES
#ifdef WRITE_PNG
/**
 * Check, if writing the osd into a png file is enabled
 */
char cSoftHdDevice::WritePngs(void)
{
	return m_pConfig->ConfigWritePngs;
};
#endif
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
 * Disables deinterlacer (called from setup menu or conf)
 */
void cSoftHdDevice::SetDisableDeint(void)
{
	if (m_pRender)
		m_pRender->DisableDeint(m_pConfig->ConfigDisableDeint);
}

/**
 * Set the passthrough mask (called from setup menu or conf)
 */
void cSoftHdDevice::SetPassthrough(int mask)
{
	m_pAudio->SetPassthrough(mask);
	if (m_pAudioDecoder)
		m_pAudioDecoder->SetPassthrough(mask);
}

/**
 * Reset the channel ID (restarts audio)
 */
void cSoftHdDevice::ResetChannelId(void)
{
	LOGDEBUG("%s:", __FUNCTION__);
	m_audioChannelID = -1;
}

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

/*****************************************************************************
 * media player functions
 ****************************************************************************/

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

	if (m_pAudio->GetFreeBytes() < AUDIO_MIN_BUFFER_FREE) {
//		LOGERROR("device: %s: m_pAudio->GetFreeBytes() < AUDIO_MIN_BUFFER_FREE!", __FUNCTION__);
		return 0;
	}
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

	if (m_pVideoStream->GetAvPacketsFilled() >= VIDEO_PACKET_MAX - 10) {
		return 0;
	}

	m_pVideoStream->PushAvPacket(pkt);

	return 1;
}

/**
 * Detach the device
 *
 * Clears audio and video, stops all threads and releases drm/alsa.
 * A detached state can only be exited (restarted) with an AttachEvent.
 */
void cSoftHdDevice::Detach(void)
{
	MakePrimaryDevice(false);
}

/**
 * Attach the device again
 *
 * Kind of a plugin restart. Inits and starts all necessary resources.
 * Only valid after a detach.
 */
void cSoftHdDevice::Attach(void)
{
	MakePrimaryDevice(true);
}

/**
 * Returns true, if the device is detached
 */
bool cSoftHdDevice::IsDetached(void) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_state == State::DETACHED;
}

/**
 * Start picture-in-picture
 */
void cSoftHdDevice::PipEnable(void)
{
	OnEventReceived(PipStartEvent{});
}

/**
 * Stop picture-in-picture
 */
void cSoftHdDevice::PipDisable(void)
{
	OnEventReceived(PipStopEvent{});
}

/**
 * Returns true, if picture-in-picture is running
 */
bool cSoftHdDevice::PipIsEnabled(void)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_pipActive;
}

/**
 * Enable/ disable picture-in-picture
 *
 * @param on       true, if pip should be enabled
 */
void cSoftHdDevice::SetEnablePip(bool on)
{
	if (m_pipActive && on) {
		LOGDEBUG("device: %s: pip is already enabled", __FUNCTION__);
		return;
	}

	if (!m_pipActive && !on) {
		LOGDEBUG("device: %s: pip is already disabled", __FUNCTION__);
		return;
	}

	if (!m_pipActive) {
		LOGDEBUG("device: %s: enabling pip", __FUNCTION__);
		NewPip(m_pipChannelNum);
		m_pRender->SetPipActive(true);
	} else {
		LOGDEBUG("device: %s: disabling pip", __FUNCTION__);
		m_pRender->SetPipActive(false);
		DelPip();
	}

	m_pipActive = on;
}

void cSoftHdDevice::DelPip(void)
{
	if (!m_pPipReceiver)
		return;

	LOGDEBUG("pip: %s: deleting receiver for channel (%d) %s", __FUNCTION__, m_pPipChannel->Number(), m_pPipChannel->Name());

	delete m_pPipReceiver;
	m_pPipReceiver = nullptr;
	m_pPipChannel = nullptr;

	m_pPipStream->DecodingThreadHalt();
	m_pRender->CancelPipFilterThread();
	m_pRender->DestroyPipFrameBuffers();
	m_pipReassemblyBuffer.Reset();
	m_pPipStream->ClearVdrCoreToDecoderQueue();
	m_pRender->ClearPipDecoderToDisplayQueue();
	m_pPipStream->CloseDecoder();
	m_pPipStream->DecodingThreadResume();
}

void cSoftHdDevice::NewPip(int channelNum)
{
	if (!channelNum)
		channelNum = CurrentChannel();

	LOCK_CHANNELS_READ;
	const cChannel *channel;
	cDevice *device;
	cPipReceiver *receiver;

	if (channelNum && (channel = Channels->GetByNumber(channelNum)) &&
	   (device = GetDevice(channel, 0, false, false))) {
		DelPip();
		device->SwitchChannel(channel, false);
		receiver = new cPipReceiver(channel, this);
		device->AttachReceiver(receiver);
		m_pPipReceiver = receiver;
		m_pPipChannel = channel;
		m_pipChannelNum = channelNum;

		LOGDEBUG("pip: %s: New receiver for channel (%d) %s", __FUNCTION__, channel->Number(), channel->Name());
	}
}
