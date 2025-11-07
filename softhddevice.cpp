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

#define _(str) gettext(str)                    ///< gettext shortcut
#define _N(str) str                            ///< gettext_noop shortcut
#define AUDIO_MIN_BUFFER_FREE (3072 * 8 * 8)   ///< Minimum free space in audio buffer 8 packets for 8 channels
#define AUDIO_BUFFER_SIZE (512 * 1024)         ///< audio PES buffer default size

/*****************************************************************************
 * audio codec parser
 ****************************************************************************/

/**
 * Mpeg bitrate table
 *
 * BitRateTable[Version][Layer][Index]
 */
static const uint16_t BitRateTable[2][4][16] = {
	// MPEG Version 1
	{{},
	{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
	{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
	{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
	// MPEG Version 2 & 2.5
	{{},
	{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
	{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
	{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}
	}
};

/**
 * Mpeg samplerate table
 */
static const uint16_t SampleRateTable[4] = {
	44100, 48000, 32000, 0
};

/**
 * Fast check for Mpeg audio
 *
 * 4 bytes 0xFFExxxxx Mpeg audio
 */
static inline int FastMpegCheck(const uint8_t * p)
{
	if (p[0] != 0xFF)               // 11bit frame sync
		return 0;
	if ((p[1] & 0xE0) != 0xE0)
		return 0;
	if ((p[1] & 0x18) == 0x08)      // version ID - 01 reserved
		return 0;
	if (!(p[1] & 0x06))             // layer description - 00 reserved
		return 0;
	if ((p[2] & 0xF0) == 0xF0)      // bitrate index - 1111 reserved
		return 0;
	if ((p[2] & 0x0C) == 0x0C)      // sampling rate index - 11 reserved
		return 0;

	return 1;
}

/**
 * Check for Mpeg audio
 *
 * 0xFFEx already checked.
 *
 * @param data     incomplete PES packet
 * @param size     number of bytes
 *
 * @retval <0      possible mpeg audio, but need more data
 * @retval 0       no valid mpeg audio
 * @retval >0      valid mpeg audio
 *
 * From: http://www.mpgedit.org/mpgedit/mpeg_format/mpeghdr.htm
 *
 * AAAAAAAA AAABBCCD EEEEFFGH IIJJKLMM
 *
 * o a 11x Frame sync
 * o b 2x	Mpeg audio version (2.5, reserved, 2, 1)
 * o c 2x	Layer (reserved, III, II, I)
 * o e 2x	BitRate index
 * o f 2x	SampleRate index (4100, 48000, 32000, 0)
 * o g 1x	Paddding bit
 * o ..	Doesn't care
 *
 * frame length:
 * Layer I:
 * 	FrameLengthInBytes = (12 * BitRate / SampleRate + Padding) * 4
 * Layer II & III:
 * 	FrameLengthInBytes = 144 * BitRate / SampleRate + Padding
 */
static int MpegCheck(const uint8_t * data, int size)
{
	int mpeg2;
	int mpeg25;
	int layer;
	int bitRateIndex;
	int sampleRateIndex;
	int padding;
	int bitRate;
	int sampleRate;
	int frameSize;

	mpeg2 = !(data[1] & 0x08) && (data[1] & 0x10);
	mpeg25 = !(data[1] & 0x08) && !(data[1] & 0x10);
	layer = 4 - ((data[1] >> 1) & 0x03);
	bitRateIndex = (data[2] >> 4) & 0x0F;
	sampleRateIndex = (data[2] >> 2) & 0x03;
	padding = (data[2] >> 1) & 0x01;

	sampleRate = SampleRateTable[sampleRateIndex];
	if (!sampleRate) {		// no valid sample rate try next (moved into fast check)
		abort();
		return 0;
	}
	sampleRate >>= mpeg2;		// mpeg 2 half rate
	sampleRate >>= mpeg25;		// mpeg 2.5 quarter rate

	bitRate = BitRateTable[mpeg2 | mpeg25][layer][bitRateIndex];
	if (!bitRate)			// no valid bit-rate try next (FIXME: move into fast check?)
		return 0;

	bitRate *= 1000;
	switch (layer) {
		case 1:
			frameSize = (12 * bitRate) / sampleRate;
			frameSize = (frameSize + padding) * 4;
			break;
		case 2:
		case 3:
		default:
			frameSize = (144 * bitRate) / sampleRate;
			frameSize = frameSize + padding;
			break;
	}

	if (frameSize + 4 > size)
		return -frameSize - 4;

	if (FastMpegCheck(data + frameSize)) {
		return frameSize;
	} else {
		LOGDEBUG("device: %s: after this frame NO new mpeg frame starts", __FUNCTION__);
		PrintStreamData(data + frameSize, frameSize);
	}

	return 0;
}

/**
 * Fast check for AAC LATM audio
 *
 * 3 bytes 0x56Exxx AAC LATM audio
 */
static inline int FastLatmCheck(const uint8_t * p)
{
	if (p[0] != 0x56) {			// 11bit sync
	return 0;
	}
	if ((p[1] & 0xE0) != 0xE0) {
	return 0;
	}
	return 1;
}

/**
 * Check for AAC LATM audio.
 *
 * 0x56Exxx already checked.
 *
 * @param data   incomplete PES packet
 * @param size   number of bytes
 *
 * @retval <0    possible AAC LATM audio, but need more data
 * @retval 0     no valid AAC LATM audio
 * @retval >0    valid AAC LATM audio
 */
static int LatmCheck(const uint8_t * data, int size)
{
	int frameSize;

	// 13 bit frame size without header
	frameSize = ((data[1] & 0x1F) << 8) + data[2];
	frameSize += 3;

	if (frameSize + 2 > size)
		return -frameSize - 2;

	// check if after this frame a new AAC LATM frame starts
	if (FastLatmCheck(data + frameSize))
		return frameSize;

	return 0;
}

/**
 * possible AC-3 frame sizes
 * from ATSC A/52 table 5.18 frame size code table.
 */
const uint16_t Ac3FrameSizeTable[38][3] = {
	{64, 69, 96}, {64, 70, 96}, {80, 87, 120}, {80, 88, 120},
	{96, 104, 144}, {96, 105, 144}, {112, 121, 168}, {112, 122, 168},
	{128, 139, 192}, {128, 140, 192}, {160, 174, 240}, {160, 175, 240},
	{192, 208, 288}, {192, 209, 288}, {224, 243, 336}, {224, 244, 336},
	{256, 278, 384}, {256, 279, 384}, {320, 348, 480}, {320, 349, 480},
	{384, 417, 576}, {384, 418, 576}, {448, 487, 672}, {448, 488, 672},
	{512, 557, 768}, {512, 558, 768}, {640, 696, 960}, {640, 697, 960},
	{768, 835, 1152}, {768, 836, 1152}, {896, 975, 1344}, {896, 976, 1344},
	{1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728},
	{1152, 1254, 1728}, {1280, 1393, 1920}, {1280, 1394, 1920},
};

/**
 * Fast check for (E-)AC-3 audio.
 *
 * 5 bytes 0x0B77xxxxxx AC-3 audio
 */
static inline int FastAc3Check(const uint8_t * p)
{
	if (p[0] != 0x0B)		// 16bit sync
		return 0;

	if (p[1] != 0x77)
		return 0;

	return 1;
}

/**
 * Check for (E-)AC-3 audio.
 *
 * 0x0B77xxxxxx already checked.
 *
 * @param data  incomplete PES packet
 * @param size  number of bytes
 *
 * @retval <0   possible AC-3 audio, but need more data
 * @retval 0    no valid AC-3 audio
 * @retval >0   valid AC-3 audio
 *
 * o AC-3 Header
 * AAAAAAAA AAAAAAAA BBBBBBBB BBBBBBBB CCDDDDDD EEEEEFFF
 *
 * o a 16x  Frame sync, always 0x0B77
 * o b 16x  CRC 16
 * o c 2x   Samplerate
 * o d 6x   Framesize code
 * o e 5x   Bitstream ID
 * o f 3x   Bitstream mode
 *
 * o E-AC-3 Header
 * AAAAAAAA AAAAAAAA BBCCCDDD DDDDDDDD EEFFGGGH IIIII...
 *
 * o a 16x  Frame sync, always 0x0B77
 * o b 2x   Frame type
 * o c 3x   Sub stream ID
 * o d 10x  Framesize - 1 in words
 * o e 2x   Framesize code
 * o f 2x   Framesize code 2
 */
static int Ac3Check(const uint8_t * data, int size)
{
	int frameSize;

	if (size < 5)                                   // need 5 bytes to see if AC-3/E-AC-3
		return -5;

	if (data[5] > (10 << 3)) {  // E-AC-3
		if ((data[4] & 0xF0) == 0xF0)               // invalid fscod fscod2
			return 0;

		frameSize = ((data[2] & 0x07) << 8) + data[3] + 1;
		frameSize *= 2;
	} else {                    // AC-3
		int fscod;
		int frmsizcod;

		// crc1 crc1 fscod|frmsizcod
		fscod = data[4] >> 6;
		if (fscod == 0x03)                          // invalid sample rate
			return 0;

		frmsizcod = data[4] & 0x3F;
		if (frmsizcod > 37)                         // invalid frame size
			return 0;

		// invalid is checked above
		frameSize = Ac3FrameSizeTable[frmsizcod][fscod] * 2;
	}

	if (frameSize + 5 > size)
		return -frameSize - 5;

	// FIXME: relaxed checks if codec is already detected
	// check if after this frame a new AC-3 frame starts
	if (FastAc3Check(data + frameSize))
		return frameSize;

	return 0;
}

/**
 * Fast check for ADTS Audio Data Transport Stream.
 *
 * 7/9 bytes 0xFFFxxxxxxxxxxx(xxxx)  ADTS audio
 */
static inline int FastAdtsCheck(const uint8_t * p)
{
	if (p[0] != 0xFF)               // 12bit sync
		return 0;

	if ((p[1] & 0xF6) != 0xF0)      // sync + layer must be 0
		return 0;

	if ((p[2] & 0x3C) == 0x3C)      // sampling frequency index != 15
		return 0;

	return 1;
}

/**
 * Check for ADTS Audio Data Transport Stream
 *
 * 0xFFF already checked.
 *
 * @param data  incomplete PES packet
 * @param size  number of bytes
 *
 * @retval <0   possible ADTS audio, but need more data
 * @retval 0    no valid ADTS audio
 * @retval >0   valid AC-3 audio
 *
 * AAAAAAAA AAAABCCD EEFFFFGH HHIJKLMM MMMMMMMM MMMOOOOO OOOOOOPP
 * (QQQQQQQQ QQQQQQQ)
 *
 * o A*12   syncword 0xFFF
 * o B*1    MPEG Version: 0 for MPEG-4, 1 for MPEG-2
 * o C*2    layer: always 0
 * o ..
 * o F*4    sampling frequency index (15 is invalid)
 * o ..
 * o M*13   frame length
 */
static int AdtsCheck(const uint8_t * data, int size)
{
	int frameSize;

	if (size < 6)
		return -6;

	frameSize = (data[3] & 0x03) << 11;
	frameSize |= (data[4] & 0xFF) << 3;
	frameSize |= (data[5] & 0xE0) >> 5;

	if (frameSize + 3 > size)
		return -frameSize - 3;

	// check if after this frame a new ADTS frame starts
	if (FastAdtsCheck(data + frameSize))
		return frameSize;

	return 0;
}

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
 * creates
 *     audio device
 *     render device
 *     video stream device
 *
 * @param config       pointer to cSoftHdConfig class
 */
cSoftHdDevice::cSoftHdDevice(cSoftHdConfig *config)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	m_pSpuDecoder = NULL;

	m_pConfig = config;
	m_pAudio = new cSoftHdAudio(this);
	m_pRender = new cVideoRender(this);
	m_pVideoStream = new cVideoStream(this);

	m_pAudioDecoder = nullptr;

	m_newAudioStream = false;
}

/**
 * cSoftHdDevice destructor
 *
 * deletes
 *    audio device
 *    render device
 *    video stream device
 */
cSoftHdDevice::~cSoftHdDevice(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	Exit();

	delete m_pVideoStream;
	delete m_pAudio;
	delete m_pRender;

	delete m_pSpuDecoder;
	LOGDEBUG("device: %s: deleted", __FUNCTION__);
}

/**
 * Device init
 *
 * prepares the renderer
 */
void cSoftHdDevice::Init()
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	m_pRender->Prepare();
}

/**
 * Device exit
 */
void cSoftHdDevice::Exit(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	m_pAudio->Exit();
	if (m_pAudioDecoder) {
		m_pAudioDecoder->Close();
		delete m_pAudioDecoder;
	}
	m_newAudioStream = false;
	av_packet_free(&m_pAudioAvPkt);

	m_pRender->Exit();
	m_pVideoStream->Exit();

	LOGDEBUG("device: %s: exited", __FUNCTION__);
}

/**
 * Device prepare
 *
 * If we don't have the audio decoder created,
 * init the audio, audio decoder and renderer
 */
void cSoftHdDevice::Start(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	if (!m_pAudioDecoder) {

		// audio
		m_pAudio->SetBufferTimeInMs(m_pConfig->ConfigAudioBufferTime);
		m_pAudioAvPkt = av_packet_alloc();
		av_new_packet(m_pAudioAvPkt, AUDIO_BUFFER_SIZE);

		m_pAudioDecoder = new cAudioDecoder(m_pAudio);
		m_audioCodecID = AV_CODEC_ID_NONE;
		m_audioChannelID = -1;

		// video stream + renderer
		if (!m_pVideoStream->Decoder())
			m_pVideoStream->SetCodecId(AV_CODEC_ID_NONE);

#ifdef USE_GLES
		if (m_pConfig->ConfigDisableOglOsd)
			m_pRender->DisableOglOsd();
#endif
		m_pRender->DisableDeint(m_pConfig->ConfigDisableDeint);
		m_pRender->Init();

		m_pVideoStream->StartDecoder(new cVideoDecoder(m_pRender));
	}
}

/**
 * Stop plugin.
 *
 * @note stop everything, but don't cleanup, module is still called.
 */
void cSoftHdDevice::Stop(void)
{
	LOGDEBUG("device: %s: nothing to do", __FUNCTION__);
}

/**
 * Clear all audio data from the decoder and ringbuffer
 */
void cSoftHdDevice::ClearAudio(void)
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	m_pAudioDecoder->FlushBuffers();
	m_pAudio->FlushBuffers();
	m_newAudioStream = true;
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
		Exit();
	else
		Start();

	cDevice::MakePrimaryDevice(on);
	if (on)
		new cSoftOsdProvider(this);
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
	if (!m_pSpuDecoder && IsPrimaryDevice())
		m_pSpuDecoder = new cDvbSpuDecoder();

	return m_pSpuDecoder;
}

/**
 * Tells whether this device has an MPEG decoder
 */
bool cSoftHdDevice::HasDecoder(void) const
{
	return true;
}

/**
 * Returns true if this device can currently start a replay session
 */
bool cSoftHdDevice::CanReplay(void) const
{
	LOGDEBUG("device: %s:", __FUNCTION__);
	return true;
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
void cSoftHdDevice::OnEventReceived(const Event& event) {
	LOGDEBUG("device: received %s", EventToString(event));

	m_pRender->DisplayThreadHalt(); // the display thread needs to be halted first, otherwise a deadlock can occur in WaitForAudioClock()
	m_pRender->DecodingThreadHalt();

	auto invalid = [this, &event]() {
		LOGWARNING("device: Invalid event '%s' in state '%s' received", EventToString(event), StateToString(m_state));
	};

	switch (m_state) {
		case State::STOP:
			std::visit(overload{
				[this](const PlayEvent&) {
					SetState(PLAY);
					m_pRender->ResetFrameCounter();
				},
				[&invalid](const PauseEvent&) { invalid(); },
				[&invalid](const StopEvent&) { invalid(); },
				[&invalid](const TrickSpeedEvent&) { invalid(); },
				[&invalid](const StillPictureEvent&) { invalid(); },
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
			}, event);
			break;
	}

	m_pRender->DecodingThreadResume();
	m_pRender->DisplayThreadResume();
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

			m_pVideoStream->ClearVdrCoreToDecoderQueue();
			m_pRender->ClearDecoderToDisplayQueue();
			m_pVideoStream->CloseDecoder();

			m_pAudio->Resume();
			ClearAudio();

			if (m_pAudioDecoder && m_audioCodecID != AV_CODEC_ID_NONE)
				m_newAudioStream = true;

			m_pVideoStream->SetInterlaced(0); // probably not necessary
			break;
		case STILL_PICTURE:
			m_pRender->SetDeinterlacerDeactivated(true);
			m_pVideoStream->SetStillPicture(true);
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
			m_pVideoStream->SetStillPicture(false);
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

	m_pRender->DisplayThreadHalt(); // the display thread needs to be halted first, otherwise a deadlock can occur in WaitForAudioClock()
	m_pRender->DecodingThreadHalt();

	m_pRender->CancelFilterThread();

	m_pVideoStream->ClearVdrCoreToDecoderQueue();
	m_pRender->ClearDecoderToDisplayQueue();

	if (m_pVideoStream->GetCodecId() != AV_CODEC_ID_NONE) // audio only (e.g. radio) has no video codec. So, don't flush the video decoder then.
		m_pVideoStream->FlushDecoder();

	m_pRender->DestroyFrameBuffers();
	m_pRender->Reset();

	ClearAudio();

	m_pRender->DisplayThreadResume();
	m_pRender->DecodingThreadResume();
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
			PlayVideo(currentPacketStart, pesPacket.GetPacketLength());
		else {
			LOGWARNING("device: %s: invalid PES packet", __FUNCTION__);
			break;
		}

		currentPacketStart += pesPacket.GetPacketLength();
	}

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
}

/**
 * Flush the device output buffers.
 *
 * @param timeout_ms        timeout in ms to become ready
 */
bool cSoftHdDevice::Flush(int timeout)
{
	LOGDEBUG("device: %s: timeout %d ms", __FUNCTION__, timeout);
	if (m_pVideoStream->GetAvPacketsFilled()) {
		if (timeout) {			// let display thread work
			usleep(timeout * 1000);
		}
		return !m_pVideoStream->GetAvPacketsFilled();
	}

	return 1;
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

	m_pVideoStream->Decoder()->GetVideoSize(&width, &height, &aspectRatio);
}

/**
 * Returns the width, height and aspect ratio the OSD
 *
 * FIXME: Called every second, for nothing (no OSD displayed)?
 */
void cSoftHdDevice::GetOsdSize(int &width, int &height, double &aspectRatio)
{
	m_pRender->GetScreenSize(&width, &height, &aspectRatio);
}

/**
 * Play an audio packet
 *
 * @param data   data of exactly one complete PES packet
 * @param size   size of PES packet
 * @param id     PES packet type
 */
int cSoftHdDevice::PlayAudio(const uchar *data, int size, uchar id)
{
//	LOGDEBUG("device: %s: %p %p %d %d", __FUNCTION__, this, data, length, id);

	int n;
	const uint8_t *p;
	AVRational timebase;
	timebase.den = 90000;
	timebase.num = 1;

	m_pAudioAvPkt->pts = AV_NOPTS_VALUE;

	m_pAudio->LazyInit();

	// hard limit buffer full: don't overrun audio buffers on replay
	if (m_pAudio->GetFreeBytes() < AUDIO_MIN_BUFFER_FREE){
//		LOGDEBUG("device: %s: Buffer is Full (%d|%d)!", __FUNCTION__, m_pAudio->GetFreeBytes(), AUDIO_MIN_BUFFER_FREE);
		return 0;
	}

	if (m_newAudioStream) {
		// this clears the audio ringbuffer indirect, open and setup does it
		LOGDEBUG("device: %s: m_newAudioStream", __FUNCTION__);
		m_pAudioDecoder->Close();
//		FlushBuffers();
//		SetBufferTimeInMs(m_pConfig->ConfigAudioBufferTime);		// ???
		m_audioCodecID = AV_CODEC_ID_NONE;
		m_audioChannelID = -1;
		m_newAudioStream = false;
	}

	// PES header 0x00 0x00 0x01 ID
	// ID 0xBD 0xC0-0xCF
	// must be a PES start code
	if (size < 9 || !data || data[0] || data[1] || data[2] != 0x01) {
		LOGERROR("device: %s: invalid PES audio packet", __FUNCTION__);
		return size;
	}
	n = data[8];			// header size

	if (size < 9 + n + 4) {		// wrong size
		if (size == 9 + n) {
			LOGWARNING("device: %s: empty audio packet", __FUNCTION__);
		} else {
			LOGERROR("device: %s: invalid audio packet %d bytes", __FUNCTION__, size);
		}
		LOGINFO("device: %s: wrong size", __FUNCTION__);
		return size;
	}

	if (data[7] & 0x80 && n >= 5) {
		m_pAudioAvPkt->pts = (int64_t) (data[9] & 0x0E) << 29 |
		                                data[10] << 22 |
		                               (data[11] & 0xFE) << 14 |
		                                data[12] << 7 |
		                               (data[13] & 0xFE) >> 1;
	//LOGDEBUG("device: %s: pts %#012" PRIx64 "\n", __FUNCTION__, m_pAudioAvPkt->pts);
	} else {
		LOGINFO("device: %s: No PTS!", __FUNCTION__);
	}

	p = data + 9 + n;
	n = size - 9 - n;			// skip pes header
	if (n + m_pAudioAvPkt->stream_index > m_pAudioAvPkt->size) {
		LOGERROR("device: %s: audio buffer too small needed %d avail %d", __FUNCTION__,
		n + m_pAudioAvPkt->stream_index, m_pAudioAvPkt->size);
		m_pAudioAvPkt->stream_index = 0;
	}

	if (m_audioChannelID != id) {		// id changed audio track changed
		m_audioChannelID = id;
		m_audioCodecID = AV_CODEC_ID_NONE;
		LOGDEBUG("device: %s: new channel id", __FUNCTION__);
	}

	// Private stream + LPCM ID
	if ((id & 0xF0) == 0xA0) {
		if (n < 7) {
			LOGERROR("device: %s: invalid LPCM audio packet %d bytes", __FUNCTION__, size);
			return size;
		}

		return size;
	}

	// DVD track header
	if ((id & 0xF0) == 0x80 && (p[0] & 0xF0) == 0x80) {
		p += 4;
		n -= 4;				// skip track header
	}

	// append new packet, to partial old data
	memcpy(m_pAudioAvPkt->data + m_pAudioAvPkt->stream_index, p, n);
	m_pAudioAvPkt->stream_index += n;

	n = m_pAudioAvPkt->stream_index;
	p = m_pAudioAvPkt->data;
	while (n >= 5) {
		int r;
		enum AVCodecID codec_id;

		// 4 bytes 0xFFExxxxx Mpeg audio
		// 3 bytes 0x56Exxx AAC LATM audio
		// 5 bytes 0x0B77xxxxxx AC-3 audio
		// 6 bytes 0x0B77xxxxxxxx E-AC-3 audio
		// 7/9 bytes 0xFFFxxxxxxxxxxx ADTS audio
		// PCM audio can't be found
		r = 0;
		codec_id = AV_CODEC_ID_NONE;	// keep compiler happy

		// AV_CODEC_ID_MP2
		if (id != 0xbd && FastMpegCheck(p)) {
			r = MpegCheck(p, n);
			codec_id = AV_CODEC_ID_MP2;
		}

		// AV_CODEC_ID_AAC_LATM
		if (id != 0xbd && !r && FastLatmCheck(p)) {
			r = LatmCheck(p, n);
			codec_id = AV_CODEC_ID_AAC_LATM;
		}

		// AV_CODEC_ID_AC3 or AV_CODEC_ID_EAC3
		if ((id == 0xbd || (id & 0xF0) == 0x80) && !r && FastAc3Check(p)) {
			r = Ac3Check(p, n);
			codec_id = AV_CODEC_ID_AC3;
			if (r > 0 && p[5] > (10 << 3)) {
			codec_id = AV_CODEC_ID_EAC3;
			}
			/* faster ac3 detection at end of pes packet (no improvemnts)
			if (m_audioCodecID == codec_id && -r - 2 == n) {
			r = n;
			}
			*/
		}

		// AV_CODEC_ID_AC3 or AV_CODEC_ID_AAC
		if (id != 0xbd && !r && FastAdtsCheck(p)) {
			r = AdtsCheck(p, n);
			codec_id = AV_CODEC_ID_AAC;
		}
		if (r < 0) {			// need more bytes
			break;
		}

		// build the packet
		if (r > 0) {
			AVPacket *avpkt;

			// new codec id, close and open new
			if (m_audioCodecID != codec_id) {
				m_pAudioDecoder->Close();
				m_pAudioDecoder->Open(codec_id, NULL, &timebase);
				m_audioCodecID = codec_id;
			}
			avpkt = av_packet_alloc();
			if (avpkt == NULL) {
				LOGERROR("device: %s: avpkt allocation failed", __FUNCTION__);
				continue;
			};
			avpkt->data = (uint8_t *)p;
			avpkt->size = r;
			avpkt->pts = m_pAudioAvPkt->pts;
			m_pAudioDecoder->Decode(avpkt);
			m_pAudioAvPkt->pts = AV_NOPTS_VALUE;
			av_packet_free(&avpkt);
			p += r;
			n -= r;
			continue;
		}
		++p;
		--n;
	}

	// copy remaining bytes to start of packet
	if (n) {
		memmove(m_pAudioAvPkt->data, p, n);
	}
	m_pAudioAvPkt->stream_index = n;

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
	LOGDEBUG("device: %s: %d", __FUNCTION__, volume);
	m_pAudio->SetVolume((volume * 1000) / 255);
}

/**
 * Read the PES header length from PES header.
 *
 * @returns length
 */
int cSoftHdDevice::PesHeadLength(const uint8_t *p)
{
  return 9 + p[8];
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
 * Play a video packet
 *
 * @param data    A complete PES packet with optionally fragmented payload
 * @param size    the length of the PES packet including header
 */
int cSoftHdDevice::PlayVideo(const uchar *data, int size)
{
	//LOGDEBUG("device: %s: %p %d", __FUNCTION__, data, size);

	if (m_pVideoStream->GetAvPacketsFilled() >= VIDEO_PACKET_MAX - 10)
		return 0;

	cPesVideo pesPacket((const uint8_t*)data, size);

	if (!pesPacket.IsValid()) {
		m_pVideoStream->ResetFragmentationBuffer();

		return size;
	}

	if (m_pVideoStream->GetCodecId() == AV_CODEC_ID_NONE) {
		// The playback has just started
		if (pesPacket.GetCodec() == AV_CODEC_ID_NONE) {
			m_pVideoStream->ResetFragmentationBuffer();

			return size;
		}

		AVCodecID codec = pesPacket.GetCodec();

		PrintStreamData(data);
		LOGDEBUG("device: %s: %s detected", __FUNCTION__, to_string(codec));

		m_pAudio->LazyInit();

		m_pVideoStream->SetCodecId(codec);
		m_pVideoStream->SetTrickpkts(codec == AV_CODEC_ID_MPEG2VIDEO ? 1 : 2);
		m_pVideoStream->Open();
		m_pVideoStream->SetTimebase(1, 90000);
	}

	m_pVideoStream->PushPesPacket(&pesPacket);

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

	// 3. get screen dimensions
	int screenwidth;
	int screenheight;
	double pixelAspect;
	m_pRender->GetScreenSize(&screenwidth, &screenheight, &pixelAspect);
	int screensize = screenwidth * screenheight * 3; // we want a RGB24

	// 4. set grab dimensions
	int grabwidth = width > 0 ? width : screenwidth;
	int grabheight = height > 0 ? height : screenheight;

	int video_size = 0;                // data size of the grabbed video
	int video_width = screenwidth;     // width of the grabbed video
	int video_height = screenheight;   // height of the grabbed video
	int video_x = 0, video_y = 0;      // x, y of the grabbed video

	// 5. fetch video data
	// Video comes as RGB, width and height is original screen dimension (video is maybe scaled)
	cSoftHdGrab *videoGrab = m_pRender->GetGrab(&video_size, &video_width, &video_height, &video_x, &video_y, 0);
	uint8_t *video = NULL;
	if (videoGrab->GetSize())
		video = videoGrab->GetData();
	if (!video) {
		LOGDEBUG2(L_GRAB, "device: %s: video is NULL, create black screen!", __FUNCTION__);
		video = (uint8_t *)calloc(1, screensize);
	}

	// 6. fetch osd data
	// OSD comes as ARGB, width and height is original screen dimension (osd is always fullscreen)
	cSoftHdGrab *osdGrab = m_pRender->GetGrab(NULL, NULL, NULL, NULL, NULL, 1);
	uint8_t *osd = NULL;
	if (osdGrab->GetSize())
		osd = osdGrab->GetData();;
	if (!osd)
		LOGDEBUG2(L_GRAB, "device: %s: osd is NULL, skip it", __FUNCTION__);

	// 7. blit the video into a full black screen if scaled
	uint8_t *scaledvideo;
	if (video_width != screenwidth || video_height != screenheight || video_x != 0 || video_y != 0) {
		scaledvideo = BlitVideo(video, screenwidth, screenheight, video_x, video_y, video_width, video_height);
		free(video);
	} else {
		scaledvideo = video;
	}

	// 8. alphablend fullscreen video with osd if available
	uint8_t *result;
	if (!osd) {
		result = scaledvideo;
	} else {
		result = (uint8_t *)malloc(screensize);
		AlphaBlend(result, osd, scaledvideo, screenwidth, screenheight);
		free(scaledvideo);
		free(osd);
	}

	// 9. scale result to requested size width + height, if it differs from fullscreen
	int scaledsize = screensize;
	uint8_t *scaledresult;
	if (screenwidth != grabwidth || screenheight != grabheight) {
		scaledresult = ScaleRgb24(result, &scaledsize, screenwidth, screenheight, grabwidth, grabheight);
		free(result);
	} else {
		scaledresult = result;
	}

	// 10. make jpeg or pnm
	uint8_t *grabbedimage;
	if (jpeg) {
		grabbedimage = CreateJpeg(scaledresult, &size, quality, grabwidth, grabheight);
	} else {  // add header to raw data
		char buf[64];
		int n = snprintf(buf, sizeof(buf), "P6\n%d\n%d\n255\n", grabwidth, grabheight);
		grabbedimage = (uint8_t *)malloc(scaledsize + n);
		memcpy(grabbedimage, buf, n);
		memcpy(grabbedimage + n, scaledresult, scaledsize);
		size = scaledsize + n;
	}
	free(scaledresult);
	LOGDEBUG2(L_GRAB, "device: %s: finished %s image (%dx%d, quality %d) at %p (size %d)", __FUNCTION__, jpeg ? "jpg" : "pnm", grabwidth, grabheight, jpeg ? quality : 0, grabbedimage, size);

	m_grabActive = false;
	return grabbedimage;
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
			m_pAudio->SetDevice(optarg);
			continue;
		case 'c':           // channel of audio mixer
			m_pAudio->SetChannel(optarg);
			continue;
		case 'p':           // pass-through audio device
			m_pAudio->SetPassthroughDevice(optarg);
			continue;
		case 'd':           // set display output
			m_pRender->SetDisplayResolution(optarg);
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
void cSoftHdDevice::SetAudioCodec(enum AVCodecID codecId, AVCodecParameters * par, AVRational * timebase)
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
void cSoftHdDevice::SetVideoCodec(enum AVCodecID codecId, AVCodecParameters * par, AVRational * timebase)
{
	m_pVideoStream->SetCodecId(codecId);
	m_pVideoStream->Open();
	m_pVideoStream->SetParameters(par);
	m_pVideoStream->SetTimebase(timebase->den, timebase->num);
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
