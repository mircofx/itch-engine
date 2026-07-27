#include "../src/core/spsc_queue.hpp"
#include <cassert>
#include <cstdio>

int main() {
	// 1. Fills to exectly CAP, then refuses
	{
		SpscQueue<int, 4> q;
		assert(q.try_push(10));
		assert(q.try_push(20));
		assert(q.try_push(30));
		assert(q.try_push(40));
		assert(!q.try_push(50));   // full at CAP=4, must refuse
	}

	// 2. FIFO order, then empty refuses
	{
		SpscQueue<int, 4> q;
		for (int i = 0; i < 4; ++i)
		{
			assert(q.try_push(i));
		}

		int v;
		for (int i = 0; i < 4; i++)
		{
			assert(q.try_pop(v));
			assert(v == i);
		}

		assert(!q.try_pop(v));
	}

	// 3. Interleave, and confirm wraparound works past CAP
	{
		SpscQueue<int, 4> q;
		int v;
		assert(q.try_push(1));
		assert(q.try_push(2));
		assert(q.try_push(3));
		assert(q.try_pop(v) && v == 1);
		assert(q.try_pop(v) && v == 2);

		assert(q.try_push(4));
		assert(q.try_push(5));
		assert(q.try_push(6));

		assert(q.try_pop(v) && v == 3);
		assert(q.try_pop(v) && v == 4);
		assert(q.try_pop(v) && v == 5);
		assert(q.try_pop(v) && v == 6);
		assert(!q.try_pop(v));
	}

	std::printf("all spsc tests passed\n");
	return 0;
}