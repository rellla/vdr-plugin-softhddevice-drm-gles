// SPDX-License-Identifier: AGLP-3.0-or-later

/**
 * @file audio.h
 * Audio and Alsa Interface Header File
 *
 * @copyright 2009 - 2014 by Johns.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

/**
 * @addtogroup audio
 * @{
 */

#ifndef __AUDIO_H
#define __AUDIO_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
}

#include <alsa/asoundlib.h>

#include <vdr/thread.h>

#include "event.h"
#include "filllevel.h"
#include "pidcontroller.h"
#include "ringbuffer.h"

class cSoftHdConfig;
class cSoftHdDevice;

/**
 * Audio Interface
 */
class cSoftHdAudio : public cThread {
public:
	cSoftHdAudio(cSoftHdDevice *);

	void LazyInit(void);
	void Exit(void);
	int Setup(AVCodecContext *, int , int , int);
	void SetPaused(bool);
	bool IsPaused(void) { return m_paused; };
	void Filter(AVFrame *, AVCodecContext *);
	void EnqueueSpdif(uint16_t *, int, AVFrame *);
	bool IsBufferFull(void) { return m_pRingbuffer.FreeBytes() <= AUDIO_MIN_BUFFER_FREE; }

	void FlushBuffers(void);
	int GetUsedBytes(void);
	int64_t GetHardwareOutputPtsMs(void);
	int64_t GetHardwareOutputDelayMs(void);
	int64_t GetHardwareOutputPtsTimebaseUnits(void);
	int GetPassthrough(void) const { return m_passthrough; }
	bool HasInputPts(void) { return m_inputPts != AV_NOPTS_VALUE; }
	int64_t GetInputPtsMs(void) { return PtsToMs(m_inputPts); }
	int64_t GetOutputPtsMs(void);
	int GetAvResyncBorderMs(void) { return AV_SYNC_BORDER_MS; };

	void SetEq(int[18], int);
	void SetVolume(int);
	void SetDownmix(int downMix) { m_downmix = downMix; };
	void SetSoftvol(bool softVolume) { m_softVolume = softVolume; };
	void SetNormalize(bool, int);
	void SetCompression(bool, int);
	void SetStereoDescent(int);
	void SetPassthroughMask(int);
	void SetAutoAES(bool appendAes) { m_appendAES = appendAes; }
	void SetTimebase(AVRational *timebase) { m_pTimebase = timebase; };

	void DropSamplesOlderThanPtsMs(int64_t);
	void ClockDriftCompensation(void);
	void ResetHwDelayBaseline(void);
	void SetHwDelayBaseline(void);

	void Stop(void);

protected:
	virtual void Action(void);

private:
	constexpr static int AUDIO_MIN_BUFFER_FREE = 3072 * 8 * 8; ///< Minimum free space in audio buffer 8 packets for 8 channels
	constexpr static int NORMALIZE_MAX_INDEX = 128;            ///< number of normalize average samples
	constexpr static int AV_SYNC_BORDER_MS = 5000;             ///< absolute max a/v difference in ms which should trigger a resync
	cSoftHdDevice *m_pDevice;               ///< pointer to device
	cSoftHdConfig *m_pConfig;               ///< pointer to config
	IEventReceiver *m_pEventReceiver;       ///< pointer to event receiver
	cBufferFillLevelLowPassFilter m_fillLevel;                  ///< low pass filter for the buffer fill level
	cPidController m_pidController{3, 0.005, 0, 1000};          ///< PID controller for clock drift compensation with tuning values coming from educated guesses
	std::chrono::steady_clock::time_point m_lastPidInvocation;  ///< last time the PID controller was invoked
	int m_alsaBufferSizeFrames = 0;         ///< alsa buffer size in frames
	int m_packetCounter = 0;                ///< packet counter for logging

	// common audio, alsa
	bool m_initialized = false;             ///< class initialized
	const int m_bytesPerSample = 2;         ///< number of bytes per sample
	unsigned int m_hwSampleRate = 0;        ///< hardware sample rate in Hz
	unsigned int m_hwNumChannels = 0;       ///< number of hardware channels
	AVRational *m_pTimebase;                ///< pointer to AVCodecContext pkts_timebase
	std::mutex m_mutex;                     ///< mutex for thread safety
	std::mutex m_pauseMutex;                ///< mutex for a safe thread pausing
	std::vector<Event> m_eventQueue;        ///< event queue for incoming events
	std::atomic<double> m_pitchPpm = 0;     ///< pitch adjustment in ppm. Positive values are faster
	int m_pitchAdjustFrameCounter = 0;      ///< counter for pitch adjustment frames

	int m_downmix;                          ///< set stereo downmix

	int64_t m_inputPts = AV_NOPTS_VALUE;    ///< pts clock (last pts in ringbuffer)
	std::atomic<bool> m_paused = true;      ///< audio is paused

	bool m_softVolume;                      ///< flag to use soft volume
	int m_passthrough;                      ///< passthrough mask
	const char *m_pPCMDevice;               ///< PCM device name
	const char *m_pPassthroughDevice;       ///< passthrough device name
	bool m_appendAES;                       ///< flag ato utomatic append AES
	int m_spdifBurstSize = 0;               ///< size of the current spdif burst
	std::vector<uint16_t> m_pauseBurst;     ///< holds the burst data itself
	snd_pcm_sframes_t m_hwBaseline = 0;     ///< saves the hw delay (pause bursts) once a real audio frame to correctly do the AV-Sync
	bool m_firstRealAudioReceived = false;  ///< false, as long as no real audio was sent - used to trigger the baseline set

	void Enqueue(uint16_t *, int, AVFrame *);
	void EnqueueFrame(AVFrame *);
	bool SendAudio(int);
	bool SendPause(void);
	void BuildPauseBurst(void);

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
	void Normalize(uint16_t *, int);

	// Compressor
	bool m_compression;                     ///< flag to use compress volume
	int m_compressionFactor = 0;            ///< current compression factor
	int m_compressionMaxFactor;             ///< max. compression factor
	void Compress(uint16_t *, int);

	// Amplifier
	int m_amplifier;                        ///< software volume amplify factor
	int m_stereoDescent;                    ///< volume descent for stereo
	int m_volume = 0;                       ///< current volume (0 .. 1000)
	void SoftAmplify(int16_t *, int);

	// Equalizer
	int m_useEqualizer;                     ///< flag to use equalizer
	float m_equalizerBand[18];              ///< equalizer band

	// mixer
	const char *m_pMixerDevice = nullptr;   ///< mixer device name (not used)
	const char *m_pMixerChannel;            ///< mixer channel name

	// filter
	int m_filterChanged = 0;                ///< filter has changed
	int m_filterReady = 0;                  ///< filter is ready
	AVFilterGraph *m_pFilterGraph = nullptr;
	AVFilterContext *m_pBuffersrcCtx;
	AVFilterContext *m_pBuffersinkCtx;
	int InitFilter(AVCodecContext *);
	AVFrame *FilterGetFrame(void);
	int CheckForFilterReady(AVCodecContext *);

	// ring buffer variables
	static constexpr unsigned RINGBUFFER_SIZE = 3 * 5 * 7 * 8 * 2 * 1000; ///< default ring buffer size ~2s 8ch 16bit (3 * 5 * 7 * 8)
	cSoftHdRingbuffer m_pRingbuffer{RINGBUFFER_SIZE};                     ///< sample ring buffer

	// alsa
	snd_pcm_t *m_pAlsaPCMHandle;         ///< alsa pcm handle
	snd_mixer_t *m_pAlsaMixer = nullptr; ///< alsa mixer handle
	snd_mixer_elem_t *m_pAlsaMixerElem = nullptr; ///< alsa mixer element
	int m_alsaRatio;                     ///< internal -> mixer ratio * 1000
	bool m_alsaUseMmap;                  ///< use mmap

	int AlsaSetup(int, int, int);
	char *OpenAlsaDevice(const char *, int);
	char *FindAlsaDevice(const char *, const char *, int);
	void AlsaInitPCMDevice(void);
	void AlsaInitMixer(void);
	void AlsaSetVolume(int);
	void AlsaInit(void);
	void AlsaExit(void);
	void FlushAlsaBuffers(void);
	void DropAlsaBuffers(void);
	void FlushAlsaBuffersInternal(bool);
	bool CyclicCall(void);
	void ProcessEvents(void);
	void HandleError(int);

	int64_t GetOutputPtsMsInternal(void);
	int64_t PtsToMs(int64_t pts) { return pts * av_q2d(*m_pTimebase) * 1000; }
	int64_t MsToPts(int64_t ptsMs) { return ptsMs / av_q2d(*m_pTimebase) / 1000; }
	int MsToFrames(int milliseconds) { return (int64_t)milliseconds * m_hwSampleRate / 1000; }
	int FramesToMs(int frames) { return (int64_t)frames * 1000 / m_hwSampleRate; }
	double FramesToMsDouble(int frames) { return (double)frames * 1000 / m_hwSampleRate; }
};

/** @} */

#endif
