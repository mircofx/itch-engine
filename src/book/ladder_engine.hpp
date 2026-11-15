#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include "../book/types.hpp"
#include "../book/order_table.hpp"
#include "../book/price_ladder.hpp"

// All the books for the feed, indexed by stock_locate, plus (TABLE_MODE 2) the one order table they share.
// apply_command() talks only to this, so the order-store layout can change without touching the decode/queue side.
//
// TABLE_MODE 0/1: forwards to books_[locate], each book owns its own order table. Same code paths as before this class existed.
// TABLE_MODE 2:   one OpenTable<GlobalSlot> keyed by order ref. Execute/cancel/delete go ref -> slot -> Level* directly,
//                 the LadderBook object is touched only on add (to place the order in the ladder) and on overflow-level cleanup.
class LadderEngine {
public:
	static constexpr size_t N_BOOKS = 65536;	// stock_locate is u16

private:
	std::vector<LadderBook> books_;
#if TABLE_MODE == 2
	OpenTable<GlobalSlot> orders_;
	uint64_t missing_ = 0;

	// Remove `removed` shares from order o; if that empties it, take it off its level and out of the table. o is invalid afterwards if the order left.
	void take_shares(GlobalSlot* o, uint32_t removed) {
		assert(removed <= o->shares);
		o->level->total_shares -= removed;
		o->shares -= removed;
		if (o->shares == 0) {
			remove_order(o);
		}
	}

	void remove_order(GlobalSlot* o) {
		Level* lvl = o->level;
		lvl->total_shares -= o->shares;		// 0 when called from take_shares, the full remainder when called from erase/replace
		lvl->order_count -= 1;
		if (o->in_overflow && lvl->order_count == 0) {
			books_[o->locate].drop_overflow_level(o->side, o->price);
		}
		orders_.erase_at(o);
	}
#endif

public:
	LadderEngine() : books_(N_BOOKS) {}

	LadderBook& operator[](size_t i) { return books_[i]; }
	const LadderBook& operator[](size_t i) const { return books_[i]; }
	size_t size() const noexcept { return books_.size(); }
	auto begin() const { return books_.begin(); }
	auto end() const { return books_.end(); }

	uint64_t missing_refs() const noexcept {
		uint64_t n = 0;
		for (const auto& b : books_) n += b.missing_refs();
#if TABLE_MODE == 2
		n += missing_;
#endif
		return n;
	}

#if TABLE_MODE == 2
	void add(uint16_t locate, uint64_t ref, char side, uint32_t price, uint32_t shares) {
		const LevelRef r = books_[locate].add_to_level(side, price, shares);
		orders_.insert(GlobalSlot{ ref, r.lvl, price, shares, locate, side, r.in_overflow });
	}

	void reduce(uint16_t locate, uint64_t ref, uint32_t shares_removed) {
		GlobalSlot* o = orders_.find(ref);
		if (o == nullptr) { ++missing_; return; }
		assert(o->locate == locate && "ITCH refs are day-unique: the message's locate must match the order's");
		(void)locate;
		take_shares(o, shares_removed);
	}

	void erase(uint16_t locate, uint64_t ref) {
		GlobalSlot* o = orders_.find(ref);
		if (o == nullptr) { ++missing_; return; }
		assert(o->locate == locate);
		(void)locate;
		remove_order(o);
	}

	void replace(uint16_t locate, uint64_t old_ref, uint64_t new_ref, uint32_t new_price, uint32_t new_shares) {
		GlobalSlot* o = orders_.find(old_ref);
		if (o == nullptr) { ++missing_; return; }
		assert(o->locate == locate);
		const char side = o->side;		// U keeps the side; copy it before remove_order() invalidates o
		remove_order(o);
		add(locate, new_ref, side, new_price, new_shares);
	}
#else
	void add(uint16_t locate, uint64_t ref, char side, uint32_t price, uint32_t shares) { books_[locate].add(ref, side, price, shares); }
	void reduce(uint16_t locate, uint64_t ref, uint32_t shares_removed) { books_[locate].reduce(ref, shares_removed); }
	void erase(uint16_t locate, uint64_t ref) { books_[locate].erase(ref); }
	void replace(uint16_t locate, uint64_t old_ref, uint64_t new_ref, uint32_t new_price, uint32_t new_shares) {
		books_[locate].replace(old_ref, new_ref, new_price, new_shares);
	}
#endif
};
