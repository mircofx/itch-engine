#pragma once
#include <unordered_map>
#include <map>
#include <vector>
#include <cstdint>
#include <cassert>

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

class OrderBook {
	std::unordered_map<uint64_t, Order> orders_;

	// price levels, sorted. bids and asks separate.
	// std::map keeps them ordered; we read the extreme end for best bid/ask.
	//   bids: best = highest price  -> rbegin()
	//   asks: best = lowest  price  -> begin()
	std::map<uint32_t, Level> bids_;
	std::map<uint32_t, Level> asks_;
	uint64_t missing_ref_count = 0;
public:
	// It can be last, inside the class body order doesn't count
	std::map<uint32_t, Level>& side_map(char side) {
		return (side == 'B') ? bids_ : asks_;
	}

	const std::map<uint32_t, Level>& side_map(char side) const {
		return (side == 'B') ? bids_ : asks_;
	}

	// message A/F
	void add(uint64_t ref, char side, uint32_t price, uint32_t shares) {
		orders_[ref] = Order{ ref, price, shares, side };

		Level& lvl = side_map(side)[price];
		lvl.total_shares += shares;
		lvl.order_count += 1;
	};

	// message X, E/C
	void reduce(uint64_t ref, uint32_t shares_removed) {
		auto it = orders_.find(ref);
		if (it == orders_.end()) {
			missing_ref_count++;
			return;
		}

		Order& o = it->second;

		assert(shares_removed <= o.shares);

		Level& lvl = side_map(o.side)[o.price];
		lvl.total_shares -= shares_removed;

		o.shares -= shares_removed;

		if (o.shares == 0) {
			lvl.order_count -= 1;
			if (lvl.order_count == 0) {
				side_map(o.side).erase(o.price);
			}
			orders_.erase(it);
		}
	};

	// message D
	void erase(uint64_t ref) {
		auto it = orders_.find(ref);

		if (it == orders_.end()) {
			missing_ref_count++;
			return;
		}

		Order& o = it->second;
		Level& lvl = side_map(o.side)[o.price];

		// removes traces of the order from the level
		lvl.total_shares -= o.shares;
		lvl.order_count -= 1;
		if (lvl.order_count == 0) {
			side_map(o.side).erase(o.price);
		}
		orders_.erase(it);
	};

	// message U
	// TODO: erase/add re-hash old_ref; fuse later
	void replace(uint64_t old_ref, uint64_t new_ref, uint32_t new_price, uint32_t new_shares) {
		auto it = orders_.find(old_ref);
		if (it == orders_.end()) {
			missing_ref_count++;
			return;
		}
		char side = it->second.side;

		erase(old_ref);
		add(new_ref, side, new_price, new_shares);
	};


	// begin() → first element = smallest key. Here, 10.00.
	// end() → one past the last.Not a real element; it's the "stop" marker. Never dereference it. it == end() is how you say "not found / empty."
	// rbegin() → reverse begin = largest key.Here, 10.03.
	// rend() → one before the first, the reverse "stop" marker.

	bool best_bid(uint32_t& price, uint64_t& shares) const {
		// last element of an ascending map
		if (bids_.empty()) {
			return false;
		}

		auto it = bids_.rbegin();
		price = it->first;
		shares = it->second.total_shares;
		return true;
	};

	bool best_ask(uint32_t& price, uint64_t& shares) const {
		// first element of an ascending map
		if (asks_.empty()) {
			return false;
		}

		auto it = asks_.begin();
		price = it->first;
		shares = it->second.total_shares;
		return true;
	};

	uint64_t missing_refs() const noexcept { return missing_ref_count; }
};