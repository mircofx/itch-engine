#pragma once
#include <cstdint>

struct Order {
	uint64_t ref;		// reduntant of map key
	uint32_t price;
	uint32_t shares;
	char side;
};

struct Level {
	uint64_t total_shares;		// sum of shares of all orders at this price
	uint32_t order_count;		// how many orders rest here
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
