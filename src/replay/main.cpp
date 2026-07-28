#include "../itch/messages.hpp"
#include "../replay/mmap_reader.hpp"
#include "../book/order_book.hpp"
#include "../book/price_ladder.hpp"
#include "../core/command.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <algorithm>

// Book mode: 0 = naive only, 1 = ladder only, 2 = both + A/B validation
#ifndef BOOK_MODE
#define BOOK_MODE 2
#endif

#define USE_NAIVE (BOOK_MODE == 0||BOOK_MODE == 2)
#define USE_LADDER (BOOK_MODE == 1||BOOK_MODE == 2)

int main(int argc, char** argv) {
	if (argc < 2) { std::fprintf(stderr, "usage: %s <itch_file>\n", argv[0]); return 1; }

	OrderTable t;
	const uint64_t N = 5000;
	std::vector<uint64_t> keys;
	// deliberately collide: multiples of a power of two >= final capacity
	for (uint64_t k = 0; k < N; ++k) keys.push_back(1 + k * 4096);
	for (uint64_t r : keys) t.insert(r, uint32_t(r % 100000), 100, 'B');
	// erase in a scattered order, then verify survivors
	for (size_t k = 0; k < keys.size(); k += 3) t.erase(keys[k]);
	uint64_t lost = 0, zombie = 0;
	for (size_t k = 0; k < keys.size(); ++k) {
		Slot* s = t.find(keys[k]);
		if (k % 3 == 0) { if (s) zombie++; }
		else { if (!s) lost++; }
	}
	std::printf("COLLIDE TEST: lost=%lu zombie=%lu\n", lost, zombie);

	MmapReader r(argv[1]);
	const uint8_t* p = r.data();
	const uint8_t* end = p + r.size();

	/*Why not a map? allocation-free, no hashing, one cache line*/
	std::array<uint64_t, 256> counts{};
	uint64_t total = 0;
	uint64_t sum_shares = 0;
	uint64_t sum_price = 0;
	uint64_t xor_refs = 0;
	uint64_t max_ts = 0;
	uint64_t sym_hash = 0;
#if USE_NAIVE
	uint64_t total_missing = 0;
#endif
#if BOOK_MODE == 2
	uint64_t mismatch_count = 0;
#endif

#if USE_NAIVE
	std::vector<OrderBook> books_naive;		// indexed by stock_locate
	books_naive.resize(65536);				// stock_locate is u16  
#endif // USE_NAIVE
#if USE_LADDER
	std::vector<LadderBook> books_fast;		// indexed by stock_locate
	books_fast.resize(65536);				// stock_locate is u16
#endif // USE_LADDER

	const auto t0 = std::chrono::steady_clock::now();
	while (p + 2 <= end) {
		/*The length prefix, not sizeof(struct), drives advancement. Unknown/unhandled types cost nothing: we jump over them without decoding*/
		const uint16_t len = static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
		p += 2;
		if (len == 0 || p + len > end) break;   // truncated tail

		/*char is signed so if will get UB if negative array index*/
		const uint8_t type = p[0];
		counts[type]++;
		++total;

		// free layout validation: on-wire len must match sizeof() for handled types
		assert(itch::payload_len(char(type)) == 0 || itch::payload_len(char(type)) == len);

		switch (type) {
		case 'A':
		{
			itch::AddOrder m;
			std::memcpy(&m, p, sizeof(m));
			max_ts = std::max(max_ts, m.h.timestamp());
			xor_refs ^= m.order_ref();
			sum_shares += m.shares();
			sum_price += m.price();
			for (char c : m.sym()) {
				sym_hash = sym_hash * 31 + uint8_t(c);
			}

#if USE_NAIVE
			books_naive[m.h.stock_locate()].add(m.order_ref(), m.side, m.price(), m.shares());
#endif
			break;
		}
		case 'F':
		{
			itch::AddOrderMPID mpid;
			std::memcpy(&mpid, p, sizeof(mpid));
			max_ts = std::max(max_ts, mpid.a.h.timestamp());
			xor_refs ^= mpid.a.order_ref();
			sum_shares += mpid.a.shares();
			sum_price += mpid.a.price();
			for (char c : mpid.a.sym()) {
				sym_hash = sym_hash * 31 + uint8_t(c);
			}

#if USE_NAIVE
			books_naive[mpid.a.h.stock_locate()].add(mpid.a.order_ref(), mpid.a.side, mpid.a.price(), mpid.a.shares());
#endif // USE_NAIVE
			break;
		}
		case 'E':
		{
			itch::OrderExecuted oe;
			std::memcpy(&oe, p, sizeof(oe));
			max_ts = std::max(max_ts, oe.h.timestamp());
			xor_refs ^= oe.order_ref();
			sum_shares += oe.exec_shares();

#if USE_NAIVE
			books_naive[oe.h.stock_locate()].reduce(oe.order_ref(), oe.exec_shares());
#endif // USE_NAIVE
			break;
		}
		case 'C':
		{
			itch::OrderExecWithPrice oewp;
			std::memcpy(&oewp, p, sizeof(oewp));
			max_ts = std::max(max_ts, oewp.e.h.timestamp());
			xor_refs ^= oewp.e.order_ref();
			sum_shares += oewp.e.exec_shares();
			sum_price += oewp.exec_price();

#if USE_NAIVE
			books_naive[oewp.e.h.stock_locate()].reduce(oewp.e.order_ref(), oewp.e.exec_shares());
#endif // USE_NAIVE
			break;
		}
		case 'X':
		{
			itch::OrderCancel oc;
			std::memcpy(&oc, p, sizeof(oc));
			max_ts = std::max(max_ts, oc.h.timestamp());
			xor_refs ^= oc.order_ref();
			sum_shares += oc.cancelled_shares();

#if USE_NAIVE
			books_naive[oc.h.stock_locate()].reduce(oc.order_ref(), oc.cancelled_shares());
#endif // USE_NAIVE
			break;
		}
		case 'D':
		{
			itch::OrderDelete od;
			std::memcpy(&od, p, sizeof(od));
			max_ts = std::max(max_ts, od.h.timestamp());
			xor_refs ^= od.order_ref();

#if USE_NAIVE
			books_naive[od.h.stock_locate()].erase(od.order_ref());
#endif // USE_NAIVE
			break;
		}
		case 'U':
		{
			itch::OrderReplace orep;
			std::memcpy(&orep, p, sizeof(orep));
			max_ts = std::max(max_ts, orep.h.timestamp());
			xor_refs ^= orep.orig_ref();
			xor_refs ^= orep.new_ref();
			sum_price += orep.price();
			sum_shares += orep.shares();

#if USE_NAIVE
			books_naive[orep.h.stock_locate()].replace(orep.orig_ref(), orep.new_ref(), orep.price(), orep.shares());
#endif // USE_NAIVE
			break;
		}
		case 'P':
		{
			itch::TradeNonCross tnc;
			std::memcpy(&tnc, p, sizeof(tnc));
			max_ts = std::max(max_ts, tnc.h.timestamp());
			xor_refs ^= tnc.order_ref();
			sum_shares += tnc.shares();
			sum_price += tnc.price();
			for (char c : tnc.sym()) {
				sym_hash = sym_hash * 31 + uint8_t(c);
			}
			break;
		}
		case 'S':
		{
			itch::SystemEvent se;                 // 12 bytes: too short for the 8-byte load
			std::memcpy(&se, p, sizeof(se));
			max_ts = std::max(max_ts, se.h.timestamp_safe());
			break;
		}
		default:
			break;
		}

#if USE_LADDER
		Command cmd;
		if (decode(p, type, cmd)) {
			apply_command(books_fast, cmd);
		}
#endif

#if BOOK_MODE == 2
		if (total % 1000000 == 0) {
			for (size_t i = 1; i < 100; ++i) {
				uint32_t p1, p2; uint64_t s1, s2;
				bool a = books_naive[i].best_bid(p1, s1);
				bool b = books_fast[i].best_bid(p2, s2);

				if (a != b || (a && (p1 != p2 || s1 != s2))) {
					mismatch_count++;
				}
			}
		}
#endif

		p += len;
	}

	const auto t1 = std::chrono::steady_clock::now();

	const double secs = std::chrono::duration<double>(t1 - t0).count();
	const double mps = double(total) / secs / 1e6;
	const double gbps = double(r.size()) / secs / 1e9;

	std::printf("messages : %lu\n", total);
	std::printf("time     : %.3f s\n", secs);
	std::printf("rate     : %.1f M msg/s   %.2f GB/s\n", mps, gbps);

	std::printf("\nper-type:\n");
	for (int i = 0; i < 256; ++i)
	{
		if (counts[i])
		{
			std::printf("  %c : %10lu  (%5.2f%%)\n", char(i), counts[i], 100.0 * double(counts[i]) / double(total));
		}
	}

	/* Order conservation : every order that rests in the book must eventually leave it.
	In: A (add), F (add w/ MPID), U (replace - creates a NEW order ref)
	Out: D (delete), X (cancel), E (execute), U (replace - kills the ORIGINAL ref)
	U sits on both sides: one replace = one removal + one insertion. Ratio should sit just above 1.0; the residual is orders still resting at EOF plus partial X/E. A large deviation means the framing walk desynchronized. */
	const uint64_t added = counts['A'] + counts['F'] + counts['U'];
	const uint64_t removed = counts['D'] + counts['X'] + counts['E'] + counts['U'];
	std::printf("\nA+F+U = %lu   D+X+E+U = %lu   ratio = %.3f\n", added, removed, double(removed) / double(added));

	std::printf("acc      : shares=%lu price=%lu refs=%lx ts=%lu sym=%lx\n", sum_shares, sum_price, xor_refs, max_ts, sym_hash);

#if USE_NAIVE
	for (const auto& b : books_naive) {
		total_missing += b.missing_refs();
	}

	std::printf("missing_refs: %lu (%.4f%% of messages)\n", total_missing, 100.0 * double(total_missing) / double(total));

	int shown = 0;
	for (size_t i = 0; i < books_naive.size() && shown < 5; ++i) {
		uint32_t bp, ap; uint64_t bs, as;
		bool hb = books_naive[i].best_bid(bp, bs);
		bool ha = books_naive[i].best_ask(ap, as);
		if (hb && ha) {
			std::printf("locate %zu: bid %u x%lu  |  ask %u x%lu  %s\n", i, bp, bs, ap, as, (bp < ap ? "ok" : "CROSSED!"));
			++shown;
		}
	}
#endif

#if USE_LADDER
	uint64_t lad_missing = 0, lad_overflow = 0, lad_subtick = 0;
	for (const auto& b : books_fast) {
		lad_missing += b.missing_refs();
		lad_overflow += b.overflows();
		lad_subtick += b.subticks();
	}
	std::printf("ladder   : missing=%lu overflow=%lu subtick=%lu\n",
		lad_missing, lad_overflow, lad_subtick);
#endif

#if BOOK_MODE == 2
	uint64_t full_mismatch = 0, lad_scans = 0;
	for (size_t i = 0; i < books_naive.size(); ++i) {
		uint32_t p1 = 0, p2 = 0, p3 = 0, p4 = 0;
		uint64_t s1 = 0, s2 = 0, s3 = 0, s4 = 0;
		bool a1 = books_naive[i].best_bid(p1, s1);
		bool b1 = books_fast[i].best_bid(p2, s2);
		bool a2 = books_naive[i].best_ask(p3, s3);
		bool b2 = books_fast[i].best_ask(p4, s4);
		if (a1 != b1 || (a1 && (p1 != p2 || s1 != s2))) {
			full_mismatch++;
		}
		if (a2 != b2 || (a2 && (p3 != p4 || s3 != s4))) {
			full_mismatch++;
		}
		lad_scans += books_fast[i].scans();
	}
	std::printf("A/B      : periodic_mismatch=%lu  final_mismatch=%lu\n", mismatch_count, full_mismatch);
	std::printf("ladder   : scan_steps=%lu\n", lad_scans);
#endif

	/* Two independently written books, running side by side over 64.9 million messages, agreed on best bid and best ask for every symbol. final_mismatch = 0.
	*/

	if (argc >= 3) {   // second arg = dump file path
		FILE* f = std::fopen(argv[2], "w");
		for (size_t i = 0; i < books_fast.size(); ++i) {   // books in threaded
			uint32_t bp, ap; uint64_t bs, as;
			bool hb = books_fast[i].best_bid(bp, bs);
			bool ha = books_fast[i].best_ask(ap, as);
			if (hb || ha)
				std::fprintf(f, "%zu %u %lu %u %lu\n", i, bp, bs, ap, as);
		}
		std::fclose(f);
	}

	return 0;
}