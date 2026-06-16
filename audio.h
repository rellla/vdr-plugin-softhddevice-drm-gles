// SPDX-License-Identifier: AGLP-3.0-or-later

/**
 * @file audio.h
 * Audio Interface Header File
 *
 * @copyright 2009 - 2014 by Johns.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

/**
 * Audio Interface
 * @defgroup audio Audio Module
 *
 * Handles the audio stream
 */

#ifndef __AUDIO_H
#define __AUDIO_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
}

#include <vdr/thread.h>

#include "alsadevice.h"
#include "audioprocessor.h"
#include "event.h"
#include "filllevel.h"
#include "pidcontroller.h"
#include "ringbuffer.h"

class cSoftHdConfig;
class cSoftHdDevice;

/**
 * Audio Interface
 *
 * @ingroup audio
 */
class cSoftHdAudio : public cThread {
public:
	cSoftHdAudio(cSoftHdDevice *);

	int LazyInit(void);
	void Exit(void);
	int Setup(AVRational, int , int , bool);
	void SetPaused(bool);
	bool IsPaused(void) { return m_paused; };
	void Filter(AVFrame *, AVCodecContext *);
	void EnqueueSpdif(const uint16_t *, int, int64_t pts);
	bool IsBufferFull(void) { return m_pRingbuffer.FreeBytes() <= AUDIO_MIN_BUFFER_FREE; };

	void FlushBuffers(void);
	int GetUsedRingbufferBytes(void);
	int GetUsedRingbufferMs(void);
	int64_t GetHardwareOutputPtsMs(void);
	int64_t GetHardwareOutputDelayMs(void);
	int64_t GetHardwareOutputPtsTimebaseUnits(void);
	bool HasInputPts(void) { return m_inputPts != AV_NOPTS_VALUE; };
	int64_t GetInputPtsMs(void) { return m_alsa.PtsToMs(m_inputPts, av_q2d(m_timebase)); };
	int64_t GetOutputPtsMs(void);
	int GetAvResyncBorderMs(void) { return AV_SYNC_BORDER_MS; };

	void SetVolume(int);
	void SetSoftvol(bool softVolume) { m_softVolume = softVolume; };

	void SetNormalize(bool, int);
	void SetCompression(bool, int);
	void SetEqualizer(bool, int[18]);
	void SetStereoDescent(int);

	void SetPassthroughMask(int mask) { m_alsa.SetPassthroughMask(mask); };
	void SetAutoAES(bool appendAes) { m_alsa.SetAutoAES(appendAes); };
	void SetTimebase(AVRational timebase) { m_timebase = timebase; };
	void SetDownmix(int downMix) { m_alsa.SetDownmix(downMix); };
	int GetPassthroughMask(void) const { return m_alsa.GetPassthroughMask(); };

	void DropSamplesOlderThanPtsMs(int64_t);
	void ClockDriftCompensation(void);
	void ResetHwDelayBaseline(void);
	void SetHwDelayBaseline(void);

protected:
	virtual void Action(void);

private:
	constexpr static int AUDIO_MIN_BUFFER_FREE = 3072 * 8 * 8; ///< Minimum free space in audio buffer 8 packets for 8 channels
	constexpr static int AV_SYNC_BORDER_MS = 5000;             ///< absolute max a/v difference in ms which should trigger a resync
	constexpr static int BYTES_PER_SAMPLE = 2;                 ///< number of bytes per sample

	cSoftHdDevice *m_pDevice;               ///< pointer to device
	cSoftHdConfig *m_pConfig;               ///< pointer to config
	cAlsaDevice m_alsa;                     ///< alsa device
	IEventReceiver *m_pEventReceiver;       ///< pointer to event receiver
	cBufferFillLevelLowPassFilter m_fillLevel;                  ///< low pass filter for the buffer fill level
	cPidController m_pidController{3, 0.005, 0, 1000};          ///< PID controller for clock drift compensation with tuning values coming from educated guesses
	std::chrono::steady_clock::time_point m_lastPidInvocation;  ///< last time the PID controller was invoked
	int m_packetCounter = 0;                ///< packet counter for logging

	// common audio
	bool m_initialized = false;             ///< class initialized
	std::mutex m_mutex;                     ///< mutex for thread safety
	std::mutex m_pauseMutex;                ///< mutex for a safe thread pausing
	std::mutex m_queueMutex;                ///< mutex for queue safety
	std::vector<Event> m_eventQueue;        ///< event queue for incoming events
	std::atomic<double> m_pitchPpm = 0;     ///< pitch adjustment in ppm. Positive values are faster
	int m_pitchAdjustFrameCounter = 0;      ///< counter for pitch adjustment frames

	int m_volume = 0;                       ///< current volume (0 .. 1000)
	int m_stereoDescent;                    ///< volume descent for stereo
	AVRational m_timebase;                  ///< AVCodecContext pkts_timebase

	int64_t m_inputPts = AV_NOPTS_VALUE;    ///< pts clock (last pts in ringbuffer)
	std::atomic<bool> m_paused = true;      ///< audio is paused

	bool m_softVolume;                      ///< flag to use soft volume
	int m_spdifBurstSize = 0;               ///< size of the current spdif burst
	std::vector<uint16_t> m_pauseBurst;     ///< holds the burst data itself
	int m_hwBaseline = 0;                   ///< saves the hw delay (pause bursts) once a real audio frame to correctly do the AV-Sync
	bool m_firstRealAudioReceived = false;  ///< false, as long as no real audio was sent - used to trigger the baseline set

	void Enqueue(const uint16_t *, int, int64_t);
	void EnqueueFrame(AVFrame *);
	bool SendAudio(int);
	bool SendPause(void);
	void BuildPauseBurst(void);
	void Stop(void);
	void FlushAlsaBuffers(void);
	void DropAlsaBuffers(void);
	bool CyclicCall(void);
	void ProcessEvents(void);

	// audio manipulation
	cAudioProcessor m_audioProcessor;
	bool m_useNormalizer;                   ///< flag to use volume normalize
	bool m_useCompressor;                   ///< flag to use compress volume
	bool m_useEqualizer;                    ///< flag to use equalizer

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
	std::string BuildChannelMapFilter(const AVChannelLayout &);

	// ring buffer variables
	static constexpr unsigned RINGBUFFER_SIZE = 3 * 5 * 7 * 8 * 2 * 1000; ///< default ring buffer size ~2s 8ch 16bit (3 * 5 * 7 * 8)
	cSoftHdRingbuffer m_pRingbuffer{RINGBUFFER_SIZE};                     ///< sample ring buffer

	int64_t GetOutputPtsMsInternal(void);
};

#endif
