// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file event.h
 * State Machine and Event Header File
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __EVENT_H
#define __EVENT_H

#include <variant>
#include <vdr/tools.h>

#include "config.h"

/********************************************************************************
 * Event Handler
 *
 * Controls the state machine.
 *******************************************************************************/

/**
 * @addtogroup misc
 * @{
 */

enum BufferUnderrunType {
	VIDEO,
	AUDIO,
};

enum PipState {
	PIPSTART,
	PIPSTOP,
	PIPTOGGLE,
	PIPCHANUP,
	PIPCHANDOWN,
	PIPCHANSWAP,
	PIPSIZECHANGE,
	PIPSWAPPOSITION
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
struct PipEvent {
	PipState state;
};
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
	PipEvent,
	ScheduleResyncAtPtsMsEvent,
	ResyncEvent,
	DisplayChangeEvent
>;

/** @} */

/**
 * Event Receiver
 *
 * @ingroup misc
 */
class IEventReceiver {
public:
	virtual void OnEventReceived(const Event&) = 0;
};

#endif
