#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <type_traits>
#include "../book/ladder_engine.hpp"
#include "../itch/messages.hpp"

// One book operation, flatten into a class. Crosses the queue between the parser thread and the book thread.
// Copyable: it gets memcpy'd into the ring buffer, so it can't own anyting or have a vtable. 
// Everything a book call needs is packed by value, nothing points back to mmap, because by the time the consumer reads it the parser has moved on.

enum : uint8_t { OP_ADD = 0, OP_REDUCE = 1, OP_ERASE = 2, OP_REPLACE = 3 };

// Flat struct, not a tagged union: every op pays for every field.
// sizeof(Command) == 40, alignof == 8 (verified: op/side/locate/price/shares/ new_price/new_shares pack to 20 bytes with zero internal gaps, then 4 bytes of pure padding to 8-align `ref`, then ref+new_ref = 16 more -> 40 total).
// Against that 40 bytes, OP_ERASE carries 11 bytes of real payload (op+locate+ref), OP_REDUCE 15 (+shares), OP_ADD 20 (+side+price+shares), OP_REPLACE all of it. At ~1.6 Commands per 64B cache line in spsc_queue's slots_ array, an
// ERASE-heavy burst is spending most of each line on fields that op doesn't use. A hand-rolled union keyed on `op` (max payload ~ OP_REPLACE's 24 bytes) would shrink this meaningfully; std::variant was ruled out here specifically because it isn't guaranteed to stay a dumb memcpy-into-ring-buffer POD the way this comment promises, kept flat for now: simpler decode()/apply_command(), and profiling hasn't shown the ring buffer's packing density as the bottleneck.
struct Command {
	uint8_t op;			// 0 = add, 1 = reduce, 2 = erase, 3 = raplace
	char side;
	uint16_t locate;	// which book (stock_locate)
	uint32_t price;
	uint32_t shares;
	uint32_t new_price;
	uint32_t new_shares;
	uint64_t ref;
	uint64_t new_ref;
};

// The consumer's only entry point for mutating the book. Only ever called on the consumer thread, so the book needs no locks: exactly one thread writes it.
inline void apply_command(LadderEngine& books, const Command& c) {
    switch (c.op) {
    case OP_ADD:
        books.add(c.locate, c.ref, c.side, c.price, c.shares);
        break;
    case OP_REDUCE:
        books.reduce(c.locate, c.ref, c.shares);
        break;
    case OP_ERASE:
        books.erase(c.locate, c.ref);
        break;
    case OP_REPLACE:
        books.replace(c.locate, c.ref, c.new_ref, c.new_price, c.new_shares);
        break;
    }
}

// Decode one ITCH message at p into Command. Return false for message type that don't touch the book (S, P, skipped types) so the caller doesn't enqueue anything.
// Also returns false for 'A'/'F' with a malformed side byte (not 'B'/'S'); see itch::is_valid_side. This is the trust boundary for the fast path: reject here, once, per message, rather than let a bad side silently misroute inside the book.
// Caller (main.cpp) is responsible for counting/logging drops if it cares to.
inline bool decode(const uint8_t* p, uint8_t type, Command& c) {
    static_assert(std::is_trivially_copyable_v<Command>);
	switch (type) {
	case 'A': {
		itch::AddOrder m;
		std::memcpy(&m, p, sizeof(m));
		if (!itch::is_valid_side(m.side)) return false;
		c = { OP_ADD, m.side, m.h.stock_locate(), m.price(), m.shares(), 0, 0, m.order_ref(), 0 };
		return true;
	}
	case 'F': {
		itch::AddOrderMPID m;
		std::memcpy(&m, p, sizeof(m));
		if (!itch::is_valid_side(m.a.side)) return false;
		c = { OP_ADD, m.a.side, m.a.h.stock_locate(), m.a.price(), m.a.shares(), 0, 0, m.a.order_ref(), 0 };
		return true;
	}
    case 'E': {
        itch::OrderExecuted m;
        std::memcpy(&m, p, sizeof(m));
        c = { OP_REDUCE, 0, m.h.stock_locate(), 0, m.exec_shares(), 0, 0, m.order_ref(), 0 };
        return true;
    }
    case 'C': {
        itch::OrderExecWithPrice m;
        std::memcpy(&m, p, sizeof(m));
        c = { OP_REDUCE, 0, m.e.h.stock_locate(), 0, m.e.exec_shares(), 0, 0, m.e.order_ref(), 0 };
        return true;
    }
    case 'X': {
        itch::OrderCancel m;
        std::memcpy(&m, p, sizeof(m));
        c = { OP_REDUCE, 0, m.h.stock_locate(), 0, m.cancelled_shares(), 0, 0, m.order_ref(), 0 };
        return true;
    }
    case 'D': {
        itch::OrderDelete m;
        std::memcpy(&m, p, sizeof(m));
        c = { OP_ERASE, 0, m.h.stock_locate(), 0, 0, 0, 0, m.order_ref(), 0 };
        return true;
    }
    case 'U': {
        itch::OrderReplace m;
        std::memcpy(&m, p, sizeof(m));
        c = { OP_REPLACE, 0, m.h.stock_locate(), 0, 0, m.price(), m.shares(), m.orig_ref(), m.new_ref() };
        return true;
    }
    default:
        return false;   // S, P, and skipped types: nothing to enqueue
    }
}