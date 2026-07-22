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
