#pragma once
#include <vector>
#include <map>
#include <cstdint>
#include <cassert>
#include "../book/types.hpp"
#include "../book/order_table.hpp"

// LadderBook (the fast one) stores price levels in a flat array where the price is the index. levels[(price - base) / 100] and you're there. One arithmetic operation, one memory access, and neighbouring price levels sit next to each other in memory, so the CPU's prefetcher works in your favour.

// Build-variant switches. #ifndef so -D on the command line wins. Every binary prints the values it was compiled with (main.cpp, main_threaded.cpp) and bench_counters.sh refuses to record a run whose printed config doesn't match what it asked for.
// History: until this guard existed, WIDTH was a hard-coded 8192 and -DLADDER_WIDTH was silently ignored, so the original width sweep compared four identical binaries.
#ifndef LADDER_WIDTH
#define LADDER_WIDTH 8192
#endif
#ifndef TABLE_MODE
#define TABLE_MODE 1        // 0 = std::unordered_map per book, 1 = OrderTable per book, 2 = one global table (ladder_engine.hpp)
#endif
#if TABLE_MODE < 0 || TABLE_MODE > 2
#error "TABLE_MODE must be 0, 1 or 2"
#endif
static_assert(LADDER_WIDTH > 0, "LADDER_WIDTH must be positive");

// What an order needs to remember about the level it rests on (TABLE_MODE 2).
struct LevelRef {
	Level* lvl;
	bool in_overflow;
};

class LadderBook {
	static constexpr uint32_t TICK = 100;
	static constexpr uint32_t WIDTH = LADDER_WIDTH;	// 8192 = circa 81.92USD window, sized to avoid overflow
	uint64_t miss_reduce_ = 0, miss_erase_ = 0, miss_replace_ = 0;

#if TABLE_MODE == 0
	MapOrderTable orders_;
#elif TABLE_MODE == 1
	OrderTable orders_;
#endif	// TABLE_MODE 2: no per-book table, LadderEngine owns the global one
	std::vector<Level> bid_levels_;
	std::vector<Level> ask_levels_;

	uint32_t base_ = 0;
	bool initialized_ = false;

	std::map<uint32_t, Level> bid_overflow_;
	std::map<uint32_t, Level> ask_overflow_;

	mutable uint32_t best_bid_hint_ = 0;		// only moves up on add(). Stale once erase() empties the top level -> O(WIDTH) scan
	mutable uint32_t best_ask_hint_ = WIDTH;	// same, mirrored for asks

	uint64_t missing_ref_count = 0;
	uint64_t overflow_count = 0;
	mutable uint64_t scan_steps = 0;
	uint64_t subtick_count = 0;

	uint32_t to_price(uint32_t idx) const { return base_ + idx * TICK; }

#ifdef PROBE_STATS
public:
	// Diagnostics only: where overflow adds land. near/mid/wide/far = caught by a window 2x/4x/16x as wide around the same centre, or not even then.
	struct OverflowStats { uint64_t subpenny = 0, below[4] = {}, above[4] = {}; };
	OverflowStats ovs;
private:
	static int ov_bucket(uint64_t ticks_outside) noexcept {
		if (ticks_outside <= WIDTH / 2)      return 0;
		if (ticks_outside <= 3 * WIDTH / 2)  return 1;
		if (ticks_outside <= 15 * WIDTH / 2) return 2;
		return 3;
	}
	void classify_overflow(uint32_t price) noexcept {
		if ((price % TICK) != 0) { ovs.subpenny++; return; }
		if (price < base_) { ovs.below[ov_bucket((base_ - price) / TICK)]++; return; }
		ovs.above[ov_bucket((uint64_t(price) - base_) / TICK - WIDTH + 1)]++;
	}
#define CLASSIFY_OVERFLOW(p) classify_overflow(p)
#else
#define CLASSIFY_OVERFLOW(p) ((void)0)
#endif

	// Single source of truth for "does this price live in the array?"
	// Must be identical on the add path and the find path, or adds and deletes route to different places and the accounting silently breaks.
	bool to_slot(uint32_t price, uint32_t& idx) const {
		if ((price % TICK) != 0)
		{
			return false;      // sub-penny: not representable
		}
		if (price < base_)
		{
			return false;
		}
		uint32_t delta = price - base_;
		if (delta >= WIDTH * TICK)
		{
			return false;
		}
		idx = delta / TICK;
		return true;
	}

	void ensure_init(uint32_t first_price) {
		if (initialized_)
		{
			return;
		}
		uint32_t half = (WIDTH / 2) * TICK;
		base_ = (first_price > half) ? (first_price - half) : 0;
		base_ = base_ - (base_ % TICK);
		bid_levels_.resize(WIDTH);
		ask_levels_.resize(WIDTH);
		initialized_ = true;
	}

	const std::vector<Level>& side_levels(char side) const {
		return (side == 'B') ? bid_levels_ : ask_levels_;
	}
	std::vector<Level>& side_levels(char side) {
		return (side == 'B') ? bid_levels_ : ask_levels_;
	}
	const std::map<uint32_t, Level>& side_overflow(char side) const {
		return (side == 'B') ? bid_overflow_ : ask_overflow_;
	}
	std::map<uint32_t, Level>& side_overflow(char side) {
		return (side == 'B') ? bid_overflow_ : ask_overflow_;
	}

	Level* find_level(char side, uint32_t price, bool& in_overflow) {
		uint32_t idx = 0;
		if (to_slot(price, idx)) {
			in_overflow = false;
			return &side_levels(side)[idx];
		}
		in_overflow = true;
		auto& ov = side_overflow(side);
		auto it = ov.find(price);
		return (it == ov.end()) ? nullptr : &it->second;
	}

public:
	// --- level-only API, used by TABLE_MODE 2 where the order lives in the global table ---
	// Same level accounting as add()/reduce()/erase() below, minus the order lookup.

	LevelRef add_to_level(char side, uint32_t price, uint32_t shares) {
		ensure_init(price);
		if ((price % TICK) != 0) subtick_count++;

		uint32_t idx = 0;
		if (to_slot(price, idx)) {
			Level& lvl = side_levels(side)[idx];
			lvl.total_shares += shares;
			lvl.order_count += 1;
			if (side == 'B' && idx > best_bid_hint_)
			{
				best_bid_hint_ = idx;
			}
			if (side == 'S' && idx < best_ask_hint_)
			{
				best_ask_hint_ = idx;
			}
			return { &lvl, false };
		}
		overflow_count++;
		CLASSIFY_OVERFLOW(price);
		Level& lvl = side_overflow(side)[price];
		lvl.total_shares += shares;
		lvl.order_count += 1;
		return { &lvl, true };
	}

	// Called only when an overflow level's last order leaves. Ladder levels are never removed, an empty one just has order_count 0.
	void drop_overflow_level(char side, uint32_t price) {
		side_overflow(side).erase(price);
	}

#if TABLE_MODE != 2
	void add(uint64_t ref, char side, uint32_t price, uint32_t shares) {
		ensure_init(price);
		if ((price % TICK) != 0) subtick_count++;

		uint32_t idx = 0;
		if (to_slot(price, idx)) {
			Level& lvl = side_levels(side)[idx];
			lvl.total_shares += shares;
			lvl.order_count += 1;
			if (side == 'B' && idx > best_bid_hint_)
			{
				best_bid_hint_ = idx;
			}
			if (side == 'S' && idx < best_ask_hint_)
			{
				best_ask_hint_ = idx;
			}
		}
		else {
			overflow_count++;
			CLASSIFY_OVERFLOW(price);
			Level& lvl = side_overflow(side)[price];
			lvl.total_shares += shares;
			lvl.order_count += 1;
		}

		orders_.insert(ref, price, shares, side);
	}

	void reduce(uint64_t ref, uint32_t shares_removed) {
		Slot* o = orders_.find(ref);
		if (o == nullptr) {
			miss_reduce_++;
			missing_ref_count++;
			return;
		}

		assert(shares_removed <= o->shares);

		bool in_ov = false;
		Level* lvl = find_level(o->side, o->price, in_ov);
		assert(lvl != nullptr);

		lvl->total_shares -= shares_removed;
		o->shares -= shares_removed;

		if (o->shares == 0) {
			lvl->order_count -= 1;
			if (lvl->order_count == 0 && in_ov) {
				side_overflow(o->side).erase(o->price);
			}
			orders_.erase(ref);
		}
	}

	void erase(uint64_t ref) {
		Slot* o = orders_.find(ref);
		if (o == nullptr)
		{
			missing_ref_count++;
			miss_erase_++;
			return;
		}

		bool in_ov = false;
		Level* lvl = find_level(o->side, o->price, in_ov);
		assert(lvl != nullptr);

		lvl->total_shares -= o->shares;
		lvl->order_count -= 1;
		if (lvl->order_count == 0 && in_ov)
		{
			side_overflow(o->side).erase(o->price);
		}
		orders_.erase(ref);
	}

	void replace(uint64_t old_ref, uint64_t new_ref, uint32_t new_price, uint32_t new_shares) {
		Slot* o = orders_.find(old_ref);
		if (o == nullptr) {
			missing_ref_count++;
			miss_replace_++;
			return;
		}

		char side = o->side;
		erase(old_ref);
		add(new_ref, side, new_price, new_shares);
	}
#endif	// TABLE_MODE != 2

	// Best price may live in the ladder OR in the overflow map. Check both, take the better. Bids want the highest price, asks the lowest.
	bool best_bid(uint32_t& price, uint64_t& shares) const {
		uint32_t lp = 0;
		uint64_t ls = 0;
		bool have_ladder = false;
		if (initialized_) {
			uint32_t i = best_bid_hint_;
			while (i > 0 && bid_levels_[i].order_count == 0) {
				--i;
				scan_steps++;
			}
			if (bid_levels_[i].order_count != 0) {
				best_bid_hint_ = i;
				lp = to_price(i);
				ls = bid_levels_[i].total_shares;
				have_ladder = true;
			}
		}

		bool have_ov = !bid_overflow_.empty();
		if (!have_ladder && !have_ov)
		{
			return false;
		}
		if (have_ov) {
			auto it = bid_overflow_.rbegin();          // highest overflow price
			if (!have_ladder || it->first > lp) {
				price = it->first; shares = it->second.total_shares;
				return true;
			}
		}
		price = lp; shares = ls; return true;
	}

	bool best_ask(uint32_t& price, uint64_t& shares) const {
		uint32_t lp = 0;
		uint64_t ls = 0;
		bool have_ladder = false;
		if (initialized_) {
			uint32_t i = best_ask_hint_;
			if (i >= WIDTH) i = WIDTH - 1;
			while (i < WIDTH - 1 && ask_levels_[i].order_count == 0) {
				++i;
				scan_steps++;
			}
			if (ask_levels_[i].order_count != 0) {
				best_ask_hint_ = i;
				lp = to_price(i);
				ls = ask_levels_[i].total_shares;
				have_ladder = true;
			}
		}

		bool have_ov = !ask_overflow_.empty();
		if (!have_ladder && !have_ov) return false;
		if (have_ov) {
			auto it = ask_overflow_.begin();           // lowest overflow price
			if (!have_ladder || it->first < lp) {
				price = it->first; shares = it->second.total_shares;
				return true;
			}
		}
		price = lp; shares = ls; return true;
	}

	uint64_t missing_refs() const noexcept { return missing_ref_count; }
	uint64_t overflows()    const noexcept { return overflow_count; }
#if defined(PROBE_STATS) && TABLE_MODE == 1
	const OrderTable& order_table() const noexcept { return orders_; }
#endif
	uint64_t subticks()     const noexcept { return subtick_count; }
	uint64_t scans()        const noexcept { return scan_steps; }
};