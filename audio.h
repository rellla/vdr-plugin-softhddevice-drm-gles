/**
 * @file audio.h
 * Audio and alsa module header file
 *
 * @copyright (c) 2009 - 2014 by Johns.  All Rights Reserved.
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

#ifndef __AUDIO_H
#define __AUDIO_H

extern "C"
{
#include <libavfilter/avfilter.h>
}

#include <alsa/asoundlib.h>
#include "ringbuffer.h"
#include "threads.h"

#define MIN_AUDIO_BUFFER    450		///< minimal output buffer in ms
#define NORMALIZE_MAX_INDEX 128		///< number of average values

class cSoftHdDevice;

/**
 * cSoftHdAudio - Audio class
 */
class cSoftHdAudio {
public:
	cSoftHdAudio(cSoftHdDevice *);
	virtual ~cSoftHdAudio(void);

	void LazyInit(void);
	void Exit(void);
	int Setup(AVCodecContext *, int , int , int);
	void Resume(void);
	void Pause(void);
	char IsPaused(void) { return m_paused; };
	char IsRunning(void) { return m_running; };
	void SetRunning(volatile char running) { m_running = running; };
	void Filter(AVFrame *, AVCodecContext *);
	void EnqueueRawData(uint16_t *, int, AVFrame *);
	int VideoReady(int64_t);
	int Skip(int64_t, int);

	void FlushBuffers(void);
	int GetUsedBytes(void);
	int GetFreeBytes(void);
	int64_t GetClock();
	int GetPassthrough(void) const { return m_passthrough; }

	void ResetNormalizer(void);
	void ResetCompressor(void);
	void SetEq(int[17], int);
	void SetVolume(int);
	void SetBufferTimeInMs(int);
	void SetDownmix(int);
	void SetSoftvol(int);
	void SetNormalize(int, int);
	void SetCompression(int, int);
	void SetStereoDescent(int);
	void SetDevice(const char *);
	void SetPassthroughDevice(const char *);
	void SetPassthrough(int);
	void SetChannel(const char *);
	void SetAutoAES(int);
	void SetTimebase(AVRational *timebase) { m_pTimebase = timebase; };

	void StopAlsaPlayer(void) { m_alsaPlayerRunning = 0; };
	void StartAlsaPlayer(void) { m_alsaPlayerRunning = 1; };
	void FlushAlsaBuffers(void);
	int PlayWithAlsa(void);
	char AlsaPlayerRunning(void) { return m_alsaPlayerRunning; };

private:
	cSoftHdDevice *m_pDevice;               ///< pointer to device

	// thread
	cAudioThread *m_pAudioThread;           ///< pointer to audio thread
	volatile char m_running;                ///< audio running / stopped
	void StartAudioThread(AVFrame *);       ///< start the audio thread

	// common audio, alsa
	bool m_initialized = false;             ///< class initialized
	const int m_bytesPerSample = 2;         ///< number of bytes per sample
	unsigned int m_hwSampleRate;            ///< hardware sample rate in Hz
	unsigned int m_hwNumChannels;           ///< number of hardware channels
	AVRational *m_pTimebase;                ///< pointer to AVCodecContext pkts_timebase

	int m_downmix;                          ///< set stereo downmix

	int64_t m_pts;                          ///< pts clock (last pts in ringbuffer)
	int m_skip;                             ///< skip m_skip audio to sync to video
	volatile char m_videoIsReady;           ///< video audio and video can by synched
	volatile char m_paused;                 ///< audio is paused

	char m_softVolume;                      ///< flag to use soft volume
	int m_passthrough;                      ///< passthrough mask
	const char *m_pPCMDevice;               ///< PCM device name
	const char *m_pPassthroughDevice;       ///< passthrough device name
	char m_appendAES;                       ///< flag ato utomatic append AES
	unsigned m_startThreshold;              ///< start play, if m_startThreshold is filled
	int m_bufferTimeInMs;                   ///< audio buffer time in ms

	// Normalizer
	char m_normalize;                       ///< flag to use volume normalize
	const int m_normalizeSamples = 4096;    ///< number of normalize samples
	int m_normalizeCounter;                 ///< normalize sample counter
	uint32_t m_normalizeAverage[NORMALIZE_MAX_INDEX]; ///< average of n last normalize sample blocks
	int m_normalizeIndex;                   ///< index into normalize average table
	int m_normalizeReady;                   ///< index normalize counter
	int m_normalizeFactor;                  ///< current normalize factor
	const int m_normalizeMinFactor = 100;   ///< min. normalize factor
	int m_normalizeMaxFactor;               ///< max. normalize factor

	// Compressor
	char m_compression;                     ///< flag to use compress volume
	int m_compressionFactor;                ///< current compression factor
	int m_compressionMaxFactor;             ///< max. compression factor

	// Amplifier
	char m_muted;                           ///< audio is muted
	int m_amplifier;                        ///< software volume amplify factor
	int m_stereoDescent;                    ///< volume descent for stereo
	int m_volume;                           ///< current volume (0 .. 1000)

	// Equalizer
	int m_useEqualizer;                     ///< flag to use equalizer
	float m_equalizerBand[18];              ///< equalizer band

	// mixer
	const char *m_pMixerDevice;             ///< mixer device name (not used)
	const char *m_pMixerChannel;            ///< mixer channel name

	// filter
	int m_filterChanged;                    ///< filter has changed
	int m_filterReady;                      ///< filter is ready
	AVFilterGraph *m_pFilterGraph;
	AVFilterContext *m_pBuffersrcCtx;
	AVFilterContext *m_pBuffersinkCtx;
	AVFrame *FilterGetFrame(void);
	int CheckForFilterReady(AVCodecContext *);

	// ring buffer variables
	cSoftHdRingbuffer *m_pRingbuffer = nullptr;                 ///< sample ring buffer
	const unsigned m_ringBufferSize = 3 * 5 * 7 * 8 * 2 * 1000; ///< default ring buffer size ~2s 8ch 16bit (3 * 5 * 7 * 8)
	cMutex m_rbMutex;                                           ///< mutex for ringbuffer access

	void Normalize(uint16_t *, int);
	void Compress(uint16_t *, int);
	void SoftAmplify(int16_t *, int);
	int InitFilter(AVCodecContext *);

	void InitRingbuffer(void);
	void ExitRingbuffer(void);

	void EnqueueFrame(AVFrame *);
	void Enqueue(uint16_t *, int, AVFrame *);

	// alsa
	snd_pcm_t *m_pAlsaPCMHandle;         ///< alsa pcm handle
	snd_mixer_t *m_pAlsaMixer;           ///< alsa mixer handle
	snd_mixer_elem_t *m_pAlsaMixerElem;  ///< alsa mixer element
	int m_alsaRatio;                     ///< internal -> mixer ratio * 1000
	char m_alsaPlayerRunning;            ///< start/ stop audio player thread
	int m_alsaUseMmap;                   ///< use mmap
	char m_alsaCanPause;                 ///< hw supports pause

	void XrunRecovery(void);
	char *OpenAlsaDevice(const char *, int);
	char *FindAlsaDevice(const char *, const char *, int);
	int AlsaSetup(int channels, int sample_rate, int passthrough);
	void AlsaInitPCMDevice(void);
	void AlsaInitMixer(void);
	void AlsaSetVolume(int);
	void AlsaInit(void);
	void AlsaExit(void);
};

#endif
