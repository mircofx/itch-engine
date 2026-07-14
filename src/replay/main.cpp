#include "../itch/messages.hpp"
#include "../replay/mmap_reader.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cassert>

int main(int argc, char** argv) {
	if (argc < 2) { std::fprintf(stderr, "usage: %s <itch_file>\n", argv[0]); return 1; }

	MmapReader r(argv[1]);
	const uint8_t* p = r.data();
	const uint8_t* end = p + r.size();

	/*Why not a map? allocation-free, no hashing, one cache line*/
	std::array<uint64_t, 256> counts{};
	uint64_t total = 0;

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
	return 0;
}