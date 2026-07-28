#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <type_traits>
#include "../book/price_ladder.hpp"
#include "../itch/messages.hpp"

// One book operation, flatten into a class. Crosses the queue between the parser thread and the book thread.
// Copyable: it gets memcpy'd into the ring buffer, so it can't own anyting or have a vtable. 
// Everything a book call needs is packed by value, nothing points back to mmap, because by the time the consumer reads it the parser has moved on.

enum : uint8_t { OP_ADD = 0, OP_REDUCE = 1, OP_ERASE = 2, OP_REPLACE = 3 };

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
inline void apply_command(std::vector<LadderBook>& books, const Command& c) {
    switch (c.op) {
    case OP_ADD: 
        books[c.locate].add(c.ref, c.side, c.price, c.shares); 
        break;
    case OP_REDUCE: 
        books[c.locate].reduce(c.ref, c.shares); 
        break;
    case OP_ERASE:
        books[c.locate].erase(c.ref);
        break;
    case OP_REPLACE:
        books[c.locate].replace(c.ref, c.new_ref, c.new_price, c.new_shares);
        break;
    }
}

// Decode one ITCH message at p into Command. Return false for message type that don't touch the book (S, P, skipped types) so the caller doesn't enqueue anything. 
inline bool decode(const uint8_t* p, uint8_t type, Command& c) {
    static_assert(std::is_trivially_copyable_v<Command>);
	switch (type) {
	case 'A': {
		itch::AddOrder m;
		std::memcpy(&m, p, sizeof(m));
		c = { OP_ADD, m.side, m.h.stock_locate(), m.price(), m.shares(), 0, 0, m.order_ref(), 0 };
		return true;
	}
	case 'F': {
		itch::AddOrderMPID m;
		std::memcpy(&m, p, sizeof(m));
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