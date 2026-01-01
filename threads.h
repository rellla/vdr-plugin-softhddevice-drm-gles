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

#include <mutex>
#include <functional>

#include "vdr/thread.h"
#include "queue.h"
#include "misc.h"
#include "drmbuffer.h"
#include "queue.h"

class cSoftHdDevice;
class cVideoStream;

/**
 * Decoding thread class
 */
class cDecodingThread : public cThread
{
public:
	cDecodingThread(cVideoStream *, const char *);
	virtual ~cDecodingThread(void);
	void Stop(void);
	void Halt(void) { m_mutex.lock(); };
	void Resume(void) { m_mutex.unlock(); };

private:
	std::mutex m_mutex;
	cVideoStream *m_pStream;

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
	void Halt(void) { m_mutex.lock(); };
	void Resume(void) { m_mutex.unlock(); };

private:
	std::mutex m_mutex;
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

private:
	cSoftHdAudio *m_pAudio;

protected:
	virtual void Action(void);
};

/**
 * Filter thread class
 */
class cFilterThread : public cThread
{
public:
	cFilterThread(cVideoRender *, cQueue<cDrmBuffer> *, const char *, std::function<void(AVFrame *)>);
	virtual ~cFilterThread(void);
	void InitAndStart(const AVCodecContext *, AVFrame *, bool);
	void Stop(void);
	void PushFrame(AVFrame *);
	bool IsInputBufferFull(void) { return m_frames.IsFull(); };
	int GetNumFramesToFilter(void) { return m_numFramesToFilter; };

private:
	cVideoRender *m_pRender;

	AVFilterGraph *m_pFilterGraph;
	AVFilterContext *m_pBuffersrcCtx;
	AVFilterContext *m_pBuffersinkCtx;

	bool m_filterBug;                               ///< flag for a ffmpeg bug
	cQueue<AVFrame> m_frames{VIDEO_SURFACES_MAX};   ///< queue for frames to be filtered
	std::function<void(AVFrame *)> m_frameOutput;   ///< function to output the frame
	cQueue<cDrmBuffer> *m_pDrmBufferQueue;          ///< pointer to renderer's DRM buffer queue
	int m_numFramesToFilter = 0;                    ///< number of frames to be filtered

	void Action(void);
	void SetFilterOutputPixFormat(AVPixelFormat);
};

#endif
