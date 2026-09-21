// Hand-off between the network thread and the game thread.
//
// docs/protocol.md §1.1: the game is single-threaded and frame-driven, and
// there's no engine tick to hook other than the frame itself. Applying net
// state straight from the socket thread would race the world update, so the
// socket thread just parks messages here, and the game thread drains them at
// a point in the frame where touching the world is actually safe.
//
// Not lock-free, and that's fine. Contention here is two threads at 25 Hz -
// a mutex is plenty, and being obviously correct matters more than shaving a
// microsecond off a frame that has 16ms to spend anyway.
#pragma once

#include <mutex>
#include <utility>
#include <vector>

namespace coopiii {

template <class T>
class SwapQueue {
public:
	void Push(T &&item) {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_items.push_back(std::move(item));
	}

	void Push(const T &item) {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_items.push_back(item);
	}

	// Moves everything pending into `out` (appended) and leaves the queue
	// empty. One lock per drain instead of one per item - the consumer then
	// gets to work on its own vector with no lock held at all.
	void DrainInto(std::vector<T> &out) {
		std::vector<T> taken;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_items.empty())
				return;
			taken.swap(m_items);
		}
		if (out.empty()) {
			out.swap(taken);
			return;
		}
		out.reserve(out.size() + taken.size());
		for (T &item : taken)
			out.push_back(std::move(item));
	}

	bool Empty() const {
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_items.empty();
	}

	size_t Size() const {
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_items.size();
	}

	void Clear() {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_items.clear();
	}

private:
	mutable std::mutex m_mutex;
	std::vector<T>     m_items;
};

} // namespace coopiii
