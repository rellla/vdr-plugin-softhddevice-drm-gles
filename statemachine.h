// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file statemachine.h
 * Device State Machine and Event Handler Header File
 *
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __STATEMACHINE_H
#define __STATEMACHINE_H

#if __cplusplus < 201703L
#error "C++17 or higher is required"
#endif

#include <atomic>
#include <mutex>
#include <variant>
#include <vector>

#include <vdr/thread.h>
#include <vdr/tools.h>

#include "config.h"
#include "logger.h"

/**
 * @addtogroup device
 * @{
 */

// State machine definitions
// Implementing C++17 visitor pattern
template<class... Ts>
struct overload : Ts... { using Ts::operator()...; };
template<class... Ts> overload(Ts...) -> overload<Ts...>;

enum State {
	STOP,
	BUFFERING,
	PLAY,
	TRICK_SPEED,
	DETACHED
};

enum BufferUnderrunType {
	VIDEO,
	AUDIO,
};

struct PlayEvent {};
struct PauseEvent {};
struct StopEvent {};
struct TrickSpeedEvent {
	double speed;
	bool active;
	bool forward;
};
struct StillPictureEvent {
	const uchar *data;
	int size;
};
struct DetachEvent {};
struct AttachEvent {};
struct BufferUnderrunEvent {
	BufferUnderrunType type;
};
struct BufferingThresholdReachedEvent {};
struct ScheduleResyncAtPtsMsEvent {
	int64_t pts;
};
struct ResyncEvent {};
struct DisplayChangeEvent {
	sDrmMode mode;
};

using Event = std::variant<
	PlayEvent,
	PauseEvent,
	StopEvent,
	TrickSpeedEvent,
	StillPictureEvent,
	DetachEvent,
	AttachEvent,
	BufferUnderrunEvent,
	BufferingThresholdReachedEvent,
	ScheduleResyncAtPtsMsEvent,
	ResyncEvent,
	DisplayChangeEvent
>;

inline const char* EventToString(const Event& e) {
	return std::visit(overload{
		[](const PlayEvent&) -> const char* { return "PlayEvent"; },
		[](const PauseEvent&) -> const char* { return "PauseEvent"; },
		[](const StopEvent&) -> const char* { return "StopEvent"; },
		[](const TrickSpeedEvent&) -> const char* { return "TrickSpeedEvent"; },
		[](const StillPictureEvent&) -> const char* { return "StillPictureEvent"; },
		[](const DetachEvent&) -> const char* { return "DetachEvent"; },
		[](const AttachEvent&) -> const char* { return "AttachEvent"; },
		[](const BufferUnderrunEvent& e) -> const char* { return e.type == AUDIO ? "BufferUnderrunEvent: Audio" : "BufferUnderrunEvent: Video"; },
		[](const BufferingThresholdReachedEvent&) -> const char* { return "BufferingThresholdReachedEvent"; },
		[](const ScheduleResyncAtPtsMsEvent&) -> const char* { return "ScheduleResyncAtPtsMsEvent"; },
		[](const ResyncEvent&) -> const char* { return "ResyncEvent"; },
		[](const DisplayChangeEvent&) -> const char* { return "DisplayChangeEvent"; },
	}, e);
}

inline const char* StateToString(State s) {
	switch(s) {
		case State::STOP: return "STOP";
		case State::BUFFERING: return "BUFFERING";
		case State::PLAY: return "PLAY";
		case State::TRICK_SPEED: return "TRICK_SPEED";
		case State::DETACHED: return "DETACHED";
	}
	return "Unknown";
}

/** @} */

class cSoftHdDevice;

/**
 * State Machine Implementation
 *
 * Events can be pushed directly to the state machine (OnEventReceived())
 * or can be pushed through the event handler queue.
 * Pushing directly may block the calling thread until OnEventReceiced()
 * returns. Pushing the event to the event handler queue avoids this locking
 * but does not guarantee, that the event is processed immediately.
 *
 * The user may choose, which solution fits best for the present use case.
 *
 *
 * @ingroup device
 */
class cStateMachine {
public:
	cStateMachine(cSoftHdDevice *);
	void OnEventReceived(const Event&);

	State GetState(void) const { return m_state; };
	void SetState(State state) { m_state = state; };
	void ChangeState(State);

private:
	cSoftHdDevice *m_pDevice;                ///< pointer to the device
	std::mutex m_mutex;                      ///< state machine mutex
	std::atomic<State> m_state = DETACHED;   ///< current state
};

/**
 * Event handler thread
 *
 * Queues events and sends them to the state machine cStateMachine as the final event receiver
 *
 * @ingroup device
 */
class cEventHandler : public cThread {
public:
	cEventHandler(cStateMachine *);
	~cEventHandler(void);
	void AddEvent(Event);

protected:
	void Action(void);

private:
	cStateMachine *m_pStateMachine;   ///< pointer to state machine
	std::mutex m_mutex;               ///< queue mutex
	std::vector<Event> m_eventQueue;  ///< event fifo queue
};

#endif
