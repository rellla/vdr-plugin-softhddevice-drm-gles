/**
 * @file threads.h
 * Thread classes header file
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

#ifndef __THREADS_H
#define __THREADS_H

#include "vdr/thread.h"
#include "queue.h"

#define VIDEO_SURFACES_MAX 3

class cSoftHdDevice;

/**
 * Decoding thread class
 */
class cDecodingThread : public cThread
{
public:
	cDecodingThread(cSoftHdDevice *);
	virtual ~cDecodingThread(void);
	void Stop(void);

private:
	cSoftHdDevice *m_pDevice;

protected:
	virtual void Action(void);
};

class cVideoRender;

/**
 * Display thread class
 */
class cDisplayThread : public cThread
{
public:
	cDisplayThread(cVideoRender *);
	virtual ~cDisplayThread(void);
	void Stop(void);

private:
	cVideoRender *m_pRender;

protected:
	virtual void Action(void);
};

/**
 * Audio thread class
 */
class cSoftHdAudio;

class cAudioThread : public cThread
{
public:
	cAudioThread(cSoftHdAudio *);
	virtual ~cAudioThread(void);
	void Stop(void);
	void SendStartSignal(void);

private:
	cSoftHdAudio *m_pAudio;
	cMutex m_mutex;
	cCondVar m_startWait;          ///< condition is triggered if audio and video is ready

protected:
	virtual void Action(void);
};

/**
 * Filter thread class
 */
class cFilterThread : public cThread
{
public:
	cFilterThread(cVideoRender *);
	virtual ~cFilterThread(void);
	int Init(const AVCodecContext *, AVFrame *, int);
	void Stop(void);
	int GetBufferFrameCount(void);
	bool PushFrame(AVFrame *);
	bool IsInterlaceFilter(void) { return m_isInterlaceFilter; };
	void WaitForIdle(void);

private:
	cVideoRender *m_pRender;

	AVFilterGraph *m_pFilterGraph;
	AVFilterContext *m_pBuffersrcCtx;
	AVFilterContext *m_pBuffersinkCtx;

	bool m_filterBug;                           ///< flag for a ffmpeg bug
	bool m_filterTrick;                         ///< the current filter handles trickspeed frames
	bool m_filterStill;                         ///< the current filter handles stillpicture frames
	bool m_isInterlaceFilter;                   ///< the current filter is an deinterlace filter

	cQueue<AVFrame *> m_frames{VIDEO_SURFACES_MAX}; ///< queue for frames to be filtered

	cCondVar m_waitIdleCondition;               ///< condition is triggered, if ringbuffer is empty

protected:
	virtual void Action(void);
};

#endif
