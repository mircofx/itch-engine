#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

template<typename T, size_t CAP>
class SpscQueue {
	static_assert((CAP& (CAP - 1)) == 0, "CAP must be power of two");
	static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable.");

	std::array<T, CAP> slots_ = {};
	static constexpr size_t MASK = CAP - 1;

	// Two indices, each on it's own cache line. Fix for false-sharing.
	// Without alignas(64), head_ and tail_ share a line, and every producer write to head_ invalidates the consumer's chached tail_ (and vice versa), bouncing one line between two cores on every single operation.
	alignas(64) std::atomic<size_t> head_{ 0 };
	alignas(64) std::atomic<size_t> tail_{ 0 };
public:
	bool try_push(const T& item) {
		auto h = head_.load(std::memory_order_relaxed);
		auto t = tail_.load(std::memory_order_acquire);

		if (h - t == CAP)
		{
			return false;
		}

		slots_[h & MASK] = item;
		head_.store(h + 1, std::memory_order_release);
		return true;
	}

	bool try_pop(T& out) {
		auto t = tail_.load(std::memory_order_relaxed);
		auto h = head_.load(std::memory_order_acquire);
		if (h == t)
		{
			return false;
		}

		out = slots_[t & MASK];
		tail_.store(t + 1, std::memory_order_release);
		return true;
	}
};