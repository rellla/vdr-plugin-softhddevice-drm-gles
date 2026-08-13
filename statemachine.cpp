// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file statemachine.cpp
 * Device State Machine and Event Handler
 *
 * This file defines cStateMachine which is the implementation
 * of a state machine for cSoftHdDevice and the event handler which
 * controls the state machine.
 *
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <atomic>
#include <mutex>
#include <variant>
#include <vector>

#include <vdr/thread.h>

#include "logger.h"
#include "softhddevice.h"
#include "statemachine.h"

/**
 * Create the state machine
 *
 * @param device       pointer to cSoftHdDevice object
 */
cStateMachine::cStateMachine(cSoftHdDevice *device)
	: m_pDevice(device)
{
}

/**
 * Sets the new state and triggers actions, which are necessary
 * when leaving the old and entering the new state.
 *
 * @param newState       target state
 */
void cStateMachine::SetState(State newState)
{
	if (m_state == newState)
		return;

	LOGDEBUG("STATE MACHINE: Preparing to leave state %s", StateToString(m_state));
	m_pDevice->LeaveState(m_state);
	LOGDEBUG("STATE MACHINE: Changing state %s -> %s", StateToString(m_state), StateToString(newState));
	m_state = newState;
	m_pDevice->EnterState(m_state);
	LOGDEBUG("STATE MACHINE: State changed to %s", StateToString(m_state));
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
void cStateMachine::OnEventReceived(const Event& event)
{
	uint64_t startStateChange = cTimeMs::Now();
	LOGDEBUG("STATE MACHINE: received %s", EventToString(event));
	bool needsResume = false;

#ifdef USE_GLES
	// Lock the GL thread before the state machine lock, because cmdCopyBufferToOutputFb() calls
	// cSoftHdDevice::OsdDrawARGB(), which itself locks the state machine mutex and we can end
	// up in a deadlock then.
	// We can safely unlock the thread again after the state change, because cSoftHdDevice::OsdDrawARGB()
	// always tests if we are in detached mode and this new state is probably set then.
	bool needsOglResume = false;
	if (m_state != DETACHED)
		needsOglResume = m_pDevice->HaltOpenGlThread();
#endif

	{ // locked state machine context
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_state != DETACHED) {
		m_pDevice->HaltVideoThreads();
		needsResume = true;
	}

	auto invalid = [this, &event]() {
		LOGWARNING("STATE MACHINE: Invalid event '%s' in state '%s' received", EventToString(event), StateToString(m_state));
	};

	switch (m_state) {
		case State::DETACHED:
			std::visit(overload{
				[&invalid](const PlayEvent&) { invalid(); },
				[&invalid](const PauseEvent&) { invalid(); },
				[&invalid](const StopEvent&) { invalid(); },
				[&invalid](const TrickSpeedEvent&) { invalid(); },
				[&invalid](const StillPictureEvent&) { invalid(); },
				[](const DetachEvent&) { /* ignore */ },
				[this](const AttachEvent&) {
					if (!m_pDevice->IsDetachForced())
						SetState(STOP);
				},
				[&invalid](const BufferUnderrunEvent&) { invalid(); },
				[&invalid](const BufferingThresholdReachedEvent&) { invalid(); },
				[&invalid](const ScheduleResyncAtPtsMsEvent&) { invalid(); },
				[&invalid](const ResyncEvent&) { invalid(); },
				[&invalid](const DisplayChangeEvent&) { invalid(); },
			}, event);
			needsResume = false;
			break;
		case State::STOP:
			std::visit(overload{
				[this](const PlayEvent&) {
					m_pDevice->InitAudio(true);
					SetState(BUFFERING);
					m_pDevice->ResetVideoFilter();
				},
				[&invalid](const PauseEvent&) { invalid(); },
				[&invalid](const StopEvent&) { invalid(); },
				[&invalid](const TrickSpeedEvent&) { invalid(); },
				[this](const StillPictureEvent& s) {
					m_pDevice->HandleStillPicture(s.data, s.size);
				},
				[this, &needsResume](const DetachEvent&) {
					SetState(DETACHED);
					needsResume = false;
				},
				[&invalid](const AttachEvent&) { invalid(); },
				[&invalid](const BufferUnderrunEvent&) { invalid(); },
				[&invalid](const BufferingThresholdReachedEvent&) { invalid(); },
				[&invalid](const ScheduleResyncAtPtsMsEvent&) { invalid(); },
				[&invalid](const ResyncEvent&) { invalid(); },
				[this](const DisplayChangeEvent& d) {
					m_pDevice->HandleDisplayModeChange(d.mode);
				},
			}, event);
			break;
		case State::BUFFERING:
			std::visit(overload{
				[this](const PlayEvent&) { /* ignore */ },
				[this](const PauseEvent&) { /* ignore */ },
				[this](const StopEvent&) {
					SetState(STOP);
				},
				[this](const TrickSpeedEvent& t) {
					// abort buffering and proceed with trick speed immediately, because trick speed shall be as fast and as demanded as possible
					SetState(PLAY);
					m_pDevice->SetTrickSpeed(t.speed, t.active, t.forward);
					SetState(TRICK_SPEED);
				},
				[this](const StillPictureEvent& s) {
					m_pDevice->HandleStillPicture(s.data, s.size);
				},
				[this, &needsResume](const DetachEvent&) {
					SetState(DETACHED);
					needsResume = false;
				},
				[&invalid](const AttachEvent&) { invalid(); },
				[&invalid](const BufferUnderrunEvent&) { invalid(); },
				[this](const BufferingThresholdReachedEvent&) {
					if (m_pDevice->SchedulePlaybackStart())
						SetState(PLAY);
				},
				[this](const ScheduleResyncAtPtsMsEvent& s) {
					SetState(PLAY);
					m_pDevice->ScheduleResyncAtPtsMs(s.pts);
				},
				[&invalid](const ResyncEvent&) { invalid(); },
				[this](const DisplayChangeEvent& d) {
					m_pDevice->HandleDisplayModeChange(d.mode);
				},
			}, event);
			break;
		case State::PLAY:
			std::visit(overload{
				[this](const PlayEvent&) {
					m_pDevice->ResumeFromPause();
				},
				[this](const PauseEvent&) {
					m_pDevice->PausePlayback(true);
				},
				[this](const StopEvent&) {
					SetState(STOP);
				},
				[this](const TrickSpeedEvent& t) {
					m_pDevice->SetTrickSpeed(t.speed, t.active, t.forward);
					SetState(TRICK_SPEED);
				},
				[this](const StillPictureEvent& s) {
					m_pDevice->HandleStillPicture(s.data, s.size);
				},
				[this, &needsResume](const DetachEvent&) {
					SetState(DETACHED);
					needsResume = false;
				},
				[&invalid](const AttachEvent&) { invalid(); },
				[this](const BufferUnderrunEvent&) {
					SetState(BUFFERING);
				},
				[&invalid](const BufferingThresholdReachedEvent&) { /* ignore */ },
				[this](const ScheduleResyncAtPtsMsEvent& s) {
					m_pDevice->ScheduleResyncAtPtsMs(s.pts);
				},
				[this](const ResyncEvent&) {
					SetState(BUFFERING);
				},
				[this](const DisplayChangeEvent& d) {
					m_pDevice->HandleDisplayModeChange(d.mode);
				},
			}, event);
			break;
		case State::TRICK_SPEED:
			std::visit(overload{
				[this](const PlayEvent&) {
					SetState(PLAY);
				},
				[this](const PauseEvent&) {
					m_pDevice->PausePlayback(false);
				},
				[this](const StopEvent&) {
					SetState(STOP);
				},
				[this](const TrickSpeedEvent& t) {
					// resume from pause, or change trick speed direction/speed
					m_pDevice->SetTrickSpeed(t.speed, t.active, t.forward);
					m_pDevice->ResumePlayback();
				},
				[this](const StillPictureEvent& s) {
					m_pDevice->HandleStillPicture(s.data, s.size);
				},
				[this, &needsResume](const DetachEvent&) {
					SetState(DETACHED);
					needsResume = false;
				},
				[&invalid](const AttachEvent&) { invalid(); },
				[this](const BufferUnderrunEvent&) { /* ignore during trick speed. Fast forward/reverse as fast and as demanded as possible */ },
				[&invalid](const BufferingThresholdReachedEvent&) { invalid(); },
				[&invalid](const ScheduleResyncAtPtsMsEvent&) { invalid(); },
				[&invalid](const ResyncEvent&) { invalid(); },
				[this](const DisplayChangeEvent& d) {
					m_pDevice->HandleDisplayModeChange(d.mode);
				},
			}, event);
			break;
	}

	if (needsResume)
		m_pDevice->ResumeVideoThreads();

	} // end of locked state machine context
#ifdef USE_GLES
	if (needsOglResume)
		m_pDevice->ResumeOpenGlThread();
#endif

	uint64_t stopStateChange = cTimeMs::Now();
	LOGDEBUG("STATE MACHINE: state change done in %d ms", (int)(stopStateChange - startStateChange));
}

/**
 * Create and start the event handler thread
 *
 * @param statemachine       pointer to the cStateMachine object as the final event receiver
 */
cEventHandler::cEventHandler(cStateMachine *statemachine)
	: cThread("event handler"),
	  m_pStateMachine(statemachine)
{
	Start();
}

/**
 * Stop and delete the event handler thread
 */
cEventHandler::~cEventHandler(void)
{
	Cancel(2);
}

/**
 * Add an event to the queue
 *
 * @param event       event, which should be added to the queue
 */
void cEventHandler::AddEvent(Event event)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_eventQueue.push_back(event);
}

/**
 * Periodically send events in the queue to the final event receiver/handler
 */
void cEventHandler::Action(void)
{
	LOGDEBUG("device: event queue handler thread started");

	while (Running()) {
		std::vector<Event> local;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			local.swap(m_eventQueue);
		}

		for (auto &event : local)
			m_pStateMachine->OnEventReceived(event);

		usleep(10000);
	}

	LOGDEBUG("device: event queue handler thread stopped");
}
