#include "../replay/mmap_reader.hpp"
#include "../book/price_ladder.hpp"
#include "../core/spsc_queue.hpp"
#include "../core/command.hpp"
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <vector>

// Parse the file, decode each message to a Command, push it. Touches only the mmap and the queue. Never the book.
static void producer(const MmapReader& r,
    SpscQueue<Command, 1 << 16>& q,
    std::atomic<bool>& done) {
    const uint8_t* p = r.data();
    const uint8_t* end = p + r.size();
    uint64_t pushed = 0;

    while (p + 2 <= end) {
        const uint16_t len = uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
        p += 2;
        if (len == 0 || p + len > end) break;   // truncated tail

        Command c;
        if (decode(p, p[0], c)) {
            while (!q.try_push(c)) {
                /* spin: queue is full, consumer is behind */
            }
            if ((++pushed % 10000000) == 0)
                std::fprintf(stderr, "producer: %lu pushed\n", pushed);
        }

        p += len;   // ALWAYS advance, even for messages decode() skips
    }

    done.store(true, std::memory_order_release);   // no more commands coming
}

// Pop Commands, apply to the book. Touches only the queue and the book.
static void consumer(std::vector<LadderBook>& books,
    SpscQueue<Command, 1 << 16>& q,
    std::atomic<bool>& done) {
    Command c;
    uint64_t popped = 0;

    for (;;) {
        if (q.try_pop(c)) {
            apply_command(books, c);
            if ((++popped % 10000000) == 0)
                std::fprintf(stderr, "consumer: %lu popped\n", popped);
        }
        else if (done.load(std::memory_order_acquire)) {
            // Producer finished and the queue looked empty. Between the failed pop and this done-check it may have pushed its last items, so drain once more before quitting, or the tail of the feed is lost.
            while (q.try_pop(c)) apply_command(books, c);
            break;   // consumer is finished
        }
        // else: queue momentarily empty, producer still running -> spin
    }
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <itch_file>\n", argv[0]);
		return 1;
	}

	MmapReader r(argv[1]);
	std::vector<LadderBook> books;
	books.resize(65536);

	SpscQueue<Command, 1 << 16> queue;
	std::atomic<bool> done{ false };

	const auto t0 = std::chrono::steady_clock::now();

	std::thread prod(producer, std::cref(r), std::ref(queue), std::ref(done));
	std::thread cons(consumer, std::ref(books), std::ref(queue), std::ref(done));

	prod.join();
	cons.join();

	const auto t1 = std::chrono::steady_clock::now();
	const double secs = std::chrono::duration<double>(t1 - t0).count();

	// best bid/ask for the first few active books, for eyeballing correctness
	std::printf("threaded done in %.3f s\n", secs);
	int shown = 0;
	for (size_t i = 0; i < books.size() && shown < 5; ++i) {
		uint32_t bp;
		uint32_t ap;
		uint64_t bs;
		uint64_t as;

		if (books[i].best_bid(bp, bs) && books[i].best_ask(ap, as)) {
			std::printf("locate %zu: bid %u x%lu | ask %u x%lu %s\n", i , bp, bs, ap, as, (bp < ap ? "ok":"CROSSED!"));
			++shown;
		}
	}

	return 0;
}