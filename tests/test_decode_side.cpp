// Regression test for the malformed-'side'-byte trust-boundary bug: decode() used to build a Command straight from whatever raw byte sat at the wire's side offset, and both order_book.hpp/price_ladder.hpp's (side == 'B') ? bids : asks ternary would silently misroute anything that wasn't 'B'/'S'. 
// Same style as test_spsc.cpp: plain asserts, no Catch2 -- this file isn't wired into CMake (no tests/CMakeLists.txt yet), compile it standalone the same way you already build test_spsc.cpp.
#include "../src/core/command.hpp"
#include <cassert>
#include <cstdio>

int main() {
	// 1. Valid side ('B') on 'A' is accepted and echoed through untouched.
	{
		itch::AddOrder m{};
		m.h.msg_type = 'A';
		m.side = 'B';
		Command c{};
		bool ok = decode(reinterpret_cast<const uint8_t*>(&m), 'A', c);
		assert(ok);
		assert(c.op == OP_ADD);
		assert(c.side == 'B');
	}

	// 2. Valid side ('S') on 'A' is accepted too -- not just a 'B' special case.
	{
		itch::AddOrder m{};
		m.h.msg_type = 'A';
		m.side = 'S';
		Command c{};
		bool ok = decode(reinterpret_cast<const uint8_t*>(&m), 'A', c);
		assert(ok);
		assert(c.side == 'S');
	}

	// 3. Malformed side (not 'B'/'S') on 'A' is rejected -- nothing enqueued.
	//    Before the fix this silently built c.side = '\0' and returned true; apply_command() would then have handed it to add(), which would have misrouted it into the ask side via the ternary.
	{
		itch::AddOrder m{};
		m.h.msg_type = 'A';
		m.side = '\0';   // corrupt/garbage byte -- never valid on the wire
		Command c{};
		bool ok = decode(reinterpret_cast<const uint8_t*>(&m), 'A', c);
		assert(!ok);
	}

	// 4. Same rejection on 'F' (AddOrderMPID) -- side lives one level deeper (m.a.side), a separate call site in decode() that needed its own gate.
	{
		itch::AddOrderMPID m{};
		m.a.h.msg_type = 'F';
		m.a.side = 'Q';  // corrupt
		Command c{};
		bool ok = decode(reinterpret_cast<const uint8_t*>(&m), 'F', c);
		assert(!ok);
	}

	// 5. is_valid_side() directly: only 'B'/'S' pass, everything else fails.
	assert(itch::is_valid_side('B'));
	assert(itch::is_valid_side('S'));
	assert(!itch::is_valid_side('X'));
	assert(!itch::is_valid_side('\0'));
	assert(!itch::is_valid_side('b'));  // wrong case is still invalid

	std::printf("all decode/side-validation tests passed\n");
	return 0;
}