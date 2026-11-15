#pragma once
#include <cstdint>

struct Order {
	uint64_t ref;		// reduntant of map key
	uint32_t price;
	uint32_t shares;
	char side;
};

// Price level of the fast ladder. LEVEL_BYTES picks the layout: 16 = 64-bit share total (padded), 8 = 32-bit share total.
// 8 is safe on real data: the largest total any level reached over the full 30 Jan 2019 day was 4,822,255 shares, 0.11% of UINT32_MAX
// (levelmax diagnostic, -DPROBE_STATS). Adds assert the total can't wrap, so a day that breaks this fails loudly in a debug build.
#ifndef LEVEL_BYTES
#define LEVEL_BYTES 8
#endif
#if LEVEL_BYTES == 16
struct Level {
	uint64_t total_shares;		// sum of shares of all orders at this price
	uint32_t order_count;		// how many orders rest here
};
#elif LEVEL_BYTES == 8
struct Level {
	uint32_t total_shares;
	uint32_t order_count;
};
#else
#error "LEVEL_BYTES must be 16 or 8"
#endif
static_assert(sizeof(Level) == LEVEL_BYTES);

// The naive reference book keeps its own always-64-bit level, so the A/B check still compares against an independent layout.
struct NaiveLevel {
	uint64_t total_shares;
	uint32_t order_count;
};

struct Slot {
	uint64_t ref;
	uint32_t price;
	uint32_t shares;
	char side;
};	// 24 bytes

// TABLE_MODE 2: one order table for the whole feed instead of one per instrument. ITCH order refs are unique across the day, so ref alone is the key.
// Each order carries a pointer straight to its price level, so execute/cancel/delete go order -> level without touching the instrument's LadderBook object.
// Same shape as WK Selph's "How to Build a Fast Limit Order Book" (Order::parentLimit, orders in one map keyed by id) and charles-cooper/itch-order-book (order_t holds book_idx + level_idx).
// Level* stays valid for the order's whole life: ladder arrays are allocated once in ensure_init() before any order rests there and never resized, and std::map nodes are stable until erased, which only happens when the level's last order leaves.
struct GlobalSlot {
	uint64_t ref;
	Level* level;
	uint32_t price;			// still needed to erase an emptied overflow level (map key)
	uint32_t shares;
	uint16_t locate;		// which book, for the rare overflow cleanup and a debug cross-check
	char side;
	bool in_overflow;
};	// 28 bytes of payload, 32 with padding: two slots per cache line instead of 2.67
static_assert(sizeof(GlobalSlot) == 32);
