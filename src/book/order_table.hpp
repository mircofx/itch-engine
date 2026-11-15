#pragma once
#include <vector>
#include <cstdint>
#include <cassert>
#include <unordered_map>
#include "../book/types.hpp"

#ifndef HASH_MODE
#define HASH_MODE 1	// 0 identity, 1 fibonacci (default: identity clusters on near-sequential refs, see paper section 7)
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

// Open-addressed table, generic over the slot type so the per-instrument table (Slot, TABLE_MODE 1) and the global one (GlobalSlot, TABLE_MODE 2) share one probing/deletion implementation.
// S needs a uint64_t `ref` member with 0 reserved as the empty marker.
template <class S>
class OpenTable {
	std::vector<S> slots_;
	uint32_t mask_ = 0;
	uint32_t size_ = 0;
	uint32_t log2_cap_ = 0;

	static constexpr uint32_t INITIAL_LOG2 = 6;

#ifdef PROBE_STATS
	// Diagnostics only (-DPROBE_STATS): slots examined per find/insert. Never in a benchmarked build.
public:
	struct ProbeStats { uint64_t calls = 0, steps = 0, max = 0; };
	ProbeStats probes;
private:
	void record(uint64_t steps) noexcept {
		probes.calls++;
		probes.steps += steps;
		if (steps > probes.max) probes.max = steps;
	}
#define PROBE_RECORD(n) record(n)
#else
#define PROBE_RECORD(n) ((void)0)
#endif

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
		std::vector<S> old = std::move(slots_);
		const uint32_t new_log2 = (log2_cap_ == 0) ? INITIAL_LOG2 : log2_cap_ + 1;
		rebuild(new_log2);
		for (const S& s : old) {
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
		slots_.assign(cap, S{});
		mask_ = cap - 1;
		size_ = 0;
	}
public:
	OpenTable() {
		rebuild(INITIAL_LOG2);
	}

	uint32_t size() const noexcept { return size_; }
	uint32_t capacity() const noexcept { return mask_ + 1; }

	S* find(uint64_t ref) noexcept {
		uint32_t i = index_of(ref);
		for (uint64_t n = 1;; ++n) {
			if (slots_[i].ref == ref) {
				PROBE_RECORD(n);
				return &slots_[i];
			}
			if (slots_[i].ref == 0) {
				PROBE_RECORD(n);
				return nullptr;
			}

			i = (i + 1) & mask_;
		}
	}

	void insert(const S& s) {
		const uint64_t ref = s.ref;
		assert(ref != 0 && "ref 0 collides with the empty sentinel");

		// Grow when this insert would push occupancy past 50%, not 80-90% like most open-addressing tables. Below 50%, a probe is more likely to land on an empty slot fast; 
		// past it, chains lengthen non-linearly, and since find() is almost always a hit (execute/ cancel/delete on a live order), that shows up as tail latency, not just average probe count. 
		// ~6MB extra at 200k orders -> cheap next to L3.
		if ((size_ + 1) * 2 >= capacity()) {
			grow();
		}

		uint32_t i = index_of(ref);
		uint64_t n = 1;
		while (slots_[i].ref != 0 && slots_[i].ref != ref) {
			i = (i + 1) & mask_;
			++n;
		}
		PROBE_RECORD(n);
		(void)n;
		if (slots_[i].ref == 0) {
			++size_;
		}
		slots_[i] = s;
	}

	bool erase(uint64_t ref) noexcept {
		S* s = find(ref);
		if (s == nullptr) {
			return false;
		}
		erase_at(s);
		return true;
	}

	// Erase a slot the caller already found, so a find-then-erase costs one probe sequence, not two. s is invalid afterwards (backward shift may move another entry into it).
	void erase_at(S* s) noexcept {
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
	}
};

#undef PROBE_RECORD

// TABLE_MODE 1: the per-instrument table, same API as before so LadderBook's call sites are unchanged.
class OrderTable : public OpenTable<Slot> {
public:
	using OpenTable<Slot>::insert;
	void insert(uint64_t ref, uint32_t price, uint32_t shares, char side) {
		OpenTable<Slot>::insert(Slot{ ref, price, shares, side });
	}
};