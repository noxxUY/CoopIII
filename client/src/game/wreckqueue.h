// The wrecks an engine seam has seen and Client has not yet put on the wire.
//
// Two of these exist and they used to be two hand-written rings of eight:
// game/vehicle.cpp's, for parked cars and session cars nobody drives, and
// game/population.cpp's, for the traffic this machine hosts. Eight was sized
// for "a rocket into a row of parked cars". BANGBANGBANG is the whole vehicle
// pool in one frame - CheatSite's handler walks every slot and calls
// BlowUpCar on each - and both rings dropped their oldest entry when full.
// Worse on the traffic side, the poll marks a car reported *before* it
// queues it, so a dropped one was never tried again: a car that burned here
// and stayed whole on every other screen for good.
//
// So the capacity is now "every car one frame can wreck", asserted where each
// queue is declared, and the ring is one piece of code, header-only so
// tools/clienttest pushes a pool's worth of wrecks through the real thing.
// Draining stays at the caller's pace (Client takes four a frame from each),
// which spreads a mass wreck over a few frames of reliable traffic instead of
// losing any of it.
#pragma once

#include "client.h"

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

// Two reports are the same car when they name the same car. The transform is
// deliberately not compared: it is where the car was when it was seen, and a
// second sighting of the same wreck is still the same wreck.
inline bool SameWreck(const UnownedBlast &a, const UnownedBlast &b) {
	return a.key.kind == b.key.kind && a.key.id == b.key.id;
}

template <size_t N>
class WreckQueue {
	static_assert(N > 0 && N <= 255, "Drain hands back a uint8_t count");

public:
	static constexpr size_t kCapacity = N;

	// False when this car is already waiting. When the queue is full the
	// oldest entry goes to make room and Dropped() counts it; the users size
	// it so that can only happen through a bug somewhere else.
	bool Push(const UnownedBlast &blast) {
		for (size_t i = 0; i < m_count; ++i)
			if (SameWreck(m_items[(m_head + i) % N], blast))
				return false;
		if (m_count == N) {
			m_head = (m_head + 1) % N;
			--m_count;
			++m_dropped;
		}
		m_items[(m_head + m_count) % N] = blast;
		++m_count;
		return true;
	}

	// Oldest first, up to `max`.
	uint8_t Drain(UnownedBlast *out, uint8_t max) {
		uint8_t n = 0;
		while (n < max && m_count > 0) {
			out[n++] = m_items[m_head];
			m_head   = (m_head + 1) % N;
			--m_count;
		}
		return n;
	}

	void Clear() {
		m_head  = 0;
		m_count = 0;
	}

	size_t   Count() const { return m_count; }
	uint32_t Dropped() const { return m_dropped; }

private:
	UnownedBlast m_items[N] = {};
	size_t       m_head     = 0;
	size_t       m_count    = 0;
	uint32_t     m_dropped  = 0;
};

} // namespace coopiii::game
