#pragma once
#include <vector>
#include <cstdint>
#include <cassert>
#include <unordered_map>
#include "../book/types.hpp"

#ifndef HASH_MODE
#define HASH_MODE 0	// 0, identity, 1 fibonacci
#endif
class MapOrderTable {
	std::unordered_map<uint64_t, Slot> m_;
public:
	Slot* find(uint64_t ref) noexcept {
		auto it = m_.find(ref);
		return (it == m_.end()) ? nullptr : &it->second;
	}
	void insert(uint64_t ref, uint32_t price, uint32_t shares, char side) {
		m_[ref] = Slot{ ref, price, shares, side };
	}
	bool erase(uint64_t ref) noexcept { return m_.erase(ref) != 0; }
	uint32_t size() const noexcept { return uint32_t(m_.size()); }
};

class OrderTable {
	std::vector<Slot> slots_;
	uint32_t mask_ = 0;
	uint32_t size_ = 0;
	uint32_t log2_cap_ = 0;

	static constexpr uint32_t INITIAL_LOG2 = 6;

	uint32_t index_of(uint64_t ref) const noexcept {
#if HASH_MODE == 0
		// identity: NASDAQ refs are near sequential, so consecutive orders land in consecutive slots. Great locality, but sequential keys plus deletion can build long clusters.
		return uint32_t(ref) & mask_;
#else
		// fibonacci mixing: one multiply, spreads keys uniformly, kills clustering, but throws away the sequential locality.
		return uint32_t((ref * 0x9E3779B97F4A7C15ull) >> (64 - log2_cap_));
#endif
	}

	static bool in_cyclic_range(uint32_t x, uint32_t lo, uint32_t hi) noexcept {
		if (lo < hi) {
			return lo < x && x <= hi;
		}
		else {
			return lo < x || x <= hi;
		}
	}

	void grow() {
		std::vector<Slot> old = std::move(slots_);
		const uint32_t new_log2 = (log2_cap_ == 0) ? INITIAL_LOG2 : log2_cap_ + 1;
		rebuild(new_log2);
		for (const Slot& s : old) {
			if (s.ref == 0) continue;
			uint32_t i = index_of(s.ref);
			while (slots_[i].ref != 0) i = (i + 1) & mask_;
			slots_[i] = s;
			++size_;
		}
	}

	void rebuild(uint32_t new_log2) {
		log2_cap_ = new_log2;
		const uint32_t cap = 1u << log2_cap_;
		slots_.assign(cap, Slot{ 0,0,0,0 });
		mask_ = cap - 1;
		size_ = 0;
	}
public:
	OrderTable() {
		rebuild(INITIAL_LOG2);
	}

	uint32_t size() const noexcept { return size_; }
	uint32_t capacity() const noexcept { return mask_ + 1; }

	Slot* find(uint64_t ref) noexcept {
		uint32_t i = index_of(ref);
		for (;;) {
			if (slots_[i].ref == ref) {
				return &slots_[i];
			}
			if (slots_[i].ref == 0) {
				return nullptr;
			}

			i=(i + 1)& mask_;
		}
	}

	void insert(uint64_t ref, uint32_t price, uint32_t shares, char side) {
		assert(ref != 0 && "ref 0 collides with the empty sentinel");

		if ((size_ + 1) * 2 >= capacity()) {
			grow();
		}

		uint32_t i = index_of(ref);
		while (slots_[i].ref != 0 && slots_[i].ref != ref) {
			i = (i + 1) & mask_;
		}
		if (slots_[i].ref == 0) {
			++size_;
		}
		slots_[i] = Slot{ ref, price, shares, side };
	}

	bool erase(uint64_t ref) noexcept {
		Slot* s = find(ref);
		if (s == nullptr) {
			return false;
		}

		uint32_t i = uint32_t(s - slots_.data());
		slots_[i].ref = 0;
		--size_;

		uint32_t j = (i + 1) & mask_;
		while (slots_[j].ref != 0) {
			const uint32_t ideal = index_of(slots_[j].ref);
			if (!in_cyclic_range(ideal, i, j)) {
				slots_[i] = slots_[j];
				slots_[j].ref = 0;
				i = j;
			}
			j = (j + 1) & mask_;
		}
		return true;
	}
};	// grow when size_ *2 >= capacity ()