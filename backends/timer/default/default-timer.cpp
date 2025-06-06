/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/scummsys.h"
#include "backends/timer/default/default-timer.h"
#include "common/util.h"
#include "common/system.h"

struct TimerSlot {
	Common::TimerManager::TimerProc callback;
	void *refCon;
	Common::String id;
	uint32 interval;	// in microseconds

	uint32 nextFireTime;	// in milliseconds
	uint32 nextFireTimeMicro;	// microseconds part of nextFire

	TimerSlot *next;

	TimerSlot() : callback(nullptr), refCon(nullptr), interval(0), nextFireTime(0), nextFireTimeMicro(0), next(nullptr) {}
};

void insertPrioQueue(TimerSlot *head, TimerSlot *newSlot) {
	// The head points to a fake anchor TimerSlot; this common
	// trick allows us to get rid of many special cases.

	const uint32 nextFireTime = newSlot->nextFireTime;
	TimerSlot *slot = head;
	newSlot->next = nullptr;

	// Insert the new slot into the sorted list of already scheduled
	// timers in such a way that the list stays sorted...
	while (true) {
		assert(slot);
		if (slot->next == nullptr || nextFireTime < slot->next->nextFireTime) {
			newSlot->next = slot->next;
			slot->next = newSlot;
			return;
		}
		slot = slot->next;
	}
}


DefaultTimerManager::DefaultTimerManager() :
	_timerCallbackNext(0),
	_head(nullptr) {

	_head = new TimerSlot();
}

DefaultTimerManager::~DefaultTimerManager() {
	Common::StackLock lock(_mutex);

	TimerSlot *slot = _head;
	while (slot) {
		TimerSlot *next = slot->next;
		delete slot;
		slot = next;
	}
	_head = nullptr;
}

void DefaultTimerManager::handler() {
	Common::StackLock lock(_mutex);

	uint32 curTime = g_system->getMillis(true);

	// On slow systems this could still be run after destructor
	if (!_head)
		return;

	// Repeat as long as there is a TimerSlot that is scheduled to fire.
	TimerSlot *slot = _head->next;
	while (slot && slot->nextFireTime < curTime) {
		// Remove the slot from the priority queue
		_head->next = slot->next;

		// Update the fire time and reinsert the TimerSlot into the priority
		// queue.
		assert(slot->interval > 0);
		slot->nextFireTime += (slot->interval / 1000);
		slot->nextFireTimeMicro += (slot->interval % 1000);
		if (slot->nextFireTimeMicro > 1000) {
			slot->nextFireTime += slot->nextFireTimeMicro / 1000;
			slot->nextFireTimeMicro %= 1000;
		}
		insertPrioQueue(_head, slot);

		// Invoke the timer callback
		assert(slot->callback);
		slot->callback(slot->refCon);

		// Look at the next scheduled timer
		slot = _head->next;
	}
}

void DefaultTimerManager::checkTimers(uint32 interval) {
	uint32 curTime = g_system->getMillis();

	// Timer checking & firing
	if (curTime >= _timerCallbackNext) {
		handler();
		_timerCallbackNext = curTime + interval;
	}
}

bool DefaultTimerManager::installTimerProc(TimerProc callback, int32 interval, void *refCon, const Common::String &id) {
	assert(interval > 0);
	Common::StackLock lock(_mutex);

	if (_callbacks.contains(id)) {
		if (_callbacks[id] != callback) {
			error("Different callbacks are referred by same name (%s)", id.c_str());
		}
	}

	for (const auto &curCallback : _callbacks) {
		if (curCallback._value == callback) {
			error("Same callback added twice (old name: %s, new name: %s)", curCallback._key.c_str(), id.c_str());
		}
	}
	_callbacks[id] = callback;

	TimerSlot *slot = new TimerSlot;
	slot->callback = callback;
	slot->refCon = refCon;
	slot->id = id;
	slot->interval = interval;
	slot->nextFireTime = g_system->getMillis() + interval / 1000;
	slot->nextFireTimeMicro = interval % 1000;
	slot->next = nullptr;

	insertPrioQueue(_head, slot);

	return true;
}

void DefaultTimerManager::removeTimerProc(TimerProc callback) {
	Common::StackLock lock(_mutex);

	TimerSlot *slot = _head;

	while (slot->next) {
		if (slot->next->callback == callback) {
			TimerSlot *next = slot->next->next;
			delete slot->next;
			slot->next = next;
		} else {
			slot = slot->next;
		}
	}

	// We need to remove all names referencing the timer proc here.
	//
	// Else we run into troubles, when the client code removes and readds timer
	// callbacks.
	//
	// Another issues occurs when one plays a game with ALSA as music driver,
	// returns to launcher and starts a different engine game with ALSA as music driver.
	// In this case the MPU401 code will add different timer procs with the
	// same name, resulting in two different callbacks added with the same
	// name and causing installTimerProc to error out.
	// A good test case is running a SCUMM with ALSA output and then a KYRA
	// game for example.
	for (TimerSlotMap::iterator i = _callbacks.begin(), end = _callbacks.end(); i != end; ++i) {
		if (i->_value == callback)
			_callbacks.erase(i);
	}
}
