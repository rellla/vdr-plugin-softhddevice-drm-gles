/**
 * @file event.h
 * State machine and event header file
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

#ifndef __EVENT_H
#define __EVENT_H

#include <variant>
#include <vdr/tools.h>

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
	int speed;
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
    PipEvent
>;

class IEventReceiver
{
public:
	virtual void OnEventReceived(const Event&) = 0;
};

#endif
