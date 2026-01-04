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

#include <atomic>
#include <vector>
#include <chrono>

extern "C"
{
#include <libavfilter/avfilter.h>
}

#include <alsa/asoundlib.h>
#include "ringbuffer.h"
#include "threads.h"
#include "event.h"
#include "config.h"
#include "filllevel.h"
#include "pidcontroller.h"

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
	void SetPaused(bool);
	bool IsPaused(void) { return m_paused; };
	void Filter(AVFrame *, AVCodecContext *);
	void Enqueue(uint16_t *, int, AVFrame *);
	bool IsBufferFull(void) { return m_pRingbuffer.FreeBytes() <= AUDIO_MIN_BUFFER_FREE; }

	void FlushBuffers(void);
	int GetUsedBytes(void);
	int GetFreeBytes(void);
	int64_t GetHardwareOutputPtsMs(void);
	int64_t GetHardwareOutputPtsTimebaseUnits(void);
	int GetPassthrough(void) const { return m_passthrough; }
	bool HasPts(void) { return m_inputPts != AV_NOPTS_VALUE; }
	int64_t GetInputPtsMs(void) { return PtsToMs(m_inputPts); }
	int64_t GetOutputPtsMs(void);

	void SetEq(int[18], int);
	void SetVolume(int);
	void SetDownmix(int downMix) { m_downmix = downMix; };
	void SetSoftvol(bool softVolume) { m_softVolume = softVolume; };
	void SetNormalize(bool, int);
	void SetCompression(bool, int);
	void SetStereoDescent(int);
	void SetPassthrough(int);
	void SetAutoAES(bool appendAes) { m_appendAES = appendAes; }
	void SetTimebase(AVRational *timebase) { m_pTimebase = timebase; };

	void FlushAlsaBuffers(void);
	bool CyclicCall(void);
	void DropSamplesOlderThanPtsMs(int64_t);
	void ProcessEvents(void);
	void ClockDriftCompensation(void);

private:
	constexpr static int AUDIO_MIN_BUFFER_FREE = 3072 * 8 * 8; ///< Minimum free space in audio buffer 8 packets for 8 channels
	cSoftHdDevice *m_pDevice;               ///< pointer to device
	cSoftHdConfig *m_pConfig;               ///< pointer to config
	IEventReceiver *m_pEventReceiver;       ///< pointer to event receiver
	cBufferFillLevelLowPassFilter m_fillLevel;                  ///< low pass filter for the buffer fill level
	cPidController m_pidController{3, 0.005, 0, 1000};          ///< PID controller for clock drift compensation with tuning values coming from educated guesses
	std::chrono::steady_clock::time_point m_lastPidInvocation;  ///< last time the PID controller was invoked
	int m_alsaBufferSizeFrames = 0;         ///< alsa buffer size in frames
	int m_packetCounter = 0;                ///< packet counter for logging

	// thread
	cAudioThread *m_pAudioThread = nullptr; ///< pointer to audio thread

	// common audio, alsa
	bool m_initialized = false;             ///< class initialized
	const int m_bytesPerSample = 2;         ///< number of bytes per sample
	unsigned int m_hwSampleRate;            ///< hardware sample rate in Hz
	unsigned int m_hwNumChannels;           ///< number of hardware channels
	AVRational *m_pTimebase;                ///< pointer to AVCodecContext pkts_timebase
	std::mutex m_mutex;                     ///< mutex for thread safety
	std::mutex m_pauseMutex;                ///< mutex for a safe thread pausing
	std::vector<Event> m_eventQueue;        ///< event queue for incoming events
	std::atomic<double> m_pitchPpm = 0;     ///< pitch adjustment in ppm. Positive values are faster
	int m_pitchAdjustFrameCounter = 0;      ///< counter for pitch adjustment frames

	int m_downmix;                          ///< set stereo downmix

	int64_t m_inputPts;                     ///< pts clock (last pts in ringbuffer)
	std::atomic<bool> m_paused = true;      ///< audio is paused

	bool m_softVolume;                      ///< flag to use soft volume
	int m_passthrough;                      ///< passthrough mask
	const char *m_pPCMDevice;               ///< PCM device name
	const char *m_pPassthroughDevice;       ///< passthrough device name
	bool m_appendAES;                       ///< flag ato utomatic append AES

	// Normalizer
	bool m_normalize;                       ///< flag to use volume normalize
	const int m_normalizeSamples = 4096;    ///< number of normalize samples
	int m_normalizeCounter;                 ///< normalize sample counter
	uint32_t m_normalizeAverage[NORMALIZE_MAX_INDEX]; ///< average of n last normalize sample blocks
	int m_normalizeIndex;                   ///< index into normalize average table
	int m_normalizeReady;                   ///< index normalize counter
	int m_normalizeFactor;                  ///< current normalize factor
	const int m_normalizeMinFactor = 100;   ///< min. normalize factor
	int m_normalizeMaxFactor;               ///< max. normalize factor

	// Compressor
	bool m_compression;                     ///< flag to use compress volume
	int m_compressionFactor;                ///< current compression factor
	int m_compressionMaxFactor;             ///< max. compression factor

	// Amplifier
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
	int m_filterChanged = 0;                ///< filter has changed
	int m_filterReady = 0;                  ///< filter is ready
	AVFilterGraph *m_pFilterGraph;
	AVFilterContext *m_pBuffersrcCtx;
	AVFilterContext *m_pBuffersinkCtx;
	AVFrame *FilterGetFrame(void);
	int CheckForFilterReady(AVCodecContext *);

	// ring buffer variables
	static constexpr unsigned RINGBUFFER_SIZE = 3 * 5 * 7 * 8 * 2 * 1000; ///< default ring buffer size ~2s 8ch 16bit (3 * 5 * 7 * 8)
	cSoftHdRingbuffer m_pRingbuffer{RINGBUFFER_SIZE};                     ///< sample ring buffer

	void Normalize(uint16_t *, int);
	void Compress(uint16_t *, int);
	void SoftAmplify(int16_t *, int);
	int InitFilter(AVCodecContext *);

	void EnqueueFrame(AVFrame *);

	// alsa
	snd_pcm_t *m_pAlsaPCMHandle;         ///< alsa pcm handle
	snd_mixer_t *m_pAlsaMixer;           ///< alsa mixer handle
	snd_mixer_elem_t *m_pAlsaMixerElem;  ///< alsa mixer element
	int m_alsaRatio;                     ///< internal -> mixer ratio * 1000
	bool m_alsaUseMmap;                  ///< use mmap

	void HandleError(int);
	char *OpenAlsaDevice(const char *, int);
	char *FindAlsaDevice(const char *, const char *, int);
	int AlsaSetup(int channels, int sample_rate, int passthrough);
	void AlsaInitPCMDevice(void);
	void AlsaInitMixer(void);
	void AlsaSetVolume(int);
	void AlsaInit(void);
	void AlsaExit(void);
	int64_t PtsToMs(int64_t pts) { return pts * av_q2d(*m_pTimebase) * 1000; }
	int64_t MsToPts(int64_t ptsMs) { return ptsMs / av_q2d(*m_pTimebase) / 1000; }
	int MsToFrames(int milliseconds) { return (int64_t)milliseconds * m_hwSampleRate / 1000; }
	int FramesToMs(int frames) { return (int64_t)frames * 1000 / m_hwSampleRate; }
	double FramesToMsDouble(int frames) { return (double)frames * 1000 / m_hwSampleRate; }

	int64_t GetOutputPtsMsInternal(void);
};

#endif
