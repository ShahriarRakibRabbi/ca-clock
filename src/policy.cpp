#include "policy.h"
#include "store.h"

#include <cassert>
#include <random>
#include <sstream>

namespace
{
	std::mt19937 g_rng;

	// Default seed left as-is; tests and experiments should explicitly seed this
	// to ensure deterministic behavior.

	// Count how many times CA-CLOCK spared a dirty page. Exposed via getter for tests.
	size_t g_dirty_spare_count = 0;

	/*
	How often the bounded scan ran out of sweeps and had to evict whatever the
	hand landed on. This number matters for honesty: if it is large, the policy is
	no longer CA-CLOCK, it is degenerating into near-random replacement because
	almost every frame is dirty and being spared. Any reported result has to be
	read alongside this counter.
	*/
	size_t g_scan_exhausted_count = 0;

	void set_error(std::string &error_message, const char *what, DWORD error_code)
	{
		std::ostringstream message;
		message << what << " failed with GetLastError()=" << error_code;
		error_message = message.str();
	}

	/*
	CLOCK's victim selection algorithm is the core page replacement policy. The hand
	rotates through the frame ring, clearing reference bits of pages it passes. When
	it finds a page with R=0, it evicts that page (after checking the dirty bit for
	the CA-CLOCK extension). If R was set, the hand clears it by demoting the page
	to PAGE_NOACCESS, which will cause the next access to fault and set R again.
	This is how software emulates a hardware reference bit.
	*/
	int select_victim_with_alpha(FrameRing &ring, PageTable &page_table, unsigned char *arena_base,
								 double alpha, int max_dirty_skips, std::string &error_message)
	{
		assert(ring.capacity > 0);
		assert(ring.hand >= 0 && ring.hand < ring.capacity);
		assert(max_dirty_skips >= 0);

		std::uniform_real_distribution<double> uniform_dist(0.0, 1.0);

		/*
		Scan bound. With max_dirty_skips = 0 this is 2*capacity, exactly as plain
		CLOCK and the probabilistic variant need: one lap to clear reference bits,
		one lap to find a victim among them.

		The bounded-k policy needs room for its own guarantee. Worst case the hand
		spends one lap clearing R bits, then meets every frame k more times while
		each spends its skip budget; on encounter k+1 a frame MUST be evicted. So
		k+2 laps is enough for the scan to always find a victim, which is what
		makes the give-up fallback unreachable for bounded-k rather than merely
		unlikely.
		*/
		const int scan_limit = (max_dirty_skips + 2) * ring.capacity;

		for (int sweep = 0; sweep < scan_limit; ++sweep)
		{
			const int frame = ring.hand;
			ring.hand = (ring.hand + 1) % ring.capacity;

			const int page_index = ring.page_of_frame[frame];
			if (page_index < 0)
			{
				// Frame is empty, use it immediately.
				return frame;
			}

			PageEntry &entry = page_table.entries[page_index];
			if (entry.referenced)
			{
				// R=1: clear it and demote the page to PAGE_NOACCESS so the next
				// access faults and sets R again.
				entry.referenced = false;
				unsigned char *page_address = arena_base + static_cast<int>(PAGE_BYTES) * page_index;
				DWORD old_protection = 0;
				if (VirtualProtect(page_address, PAGE_BYTES, PAGE_NOACCESS, &old_protection) == 0)
				{
					set_error(error_message, "VirtualProtect", GetLastError());
					return -1;
				}
				continue;
			}

			/*
			R = 0. This is the ONLY place the three policies differ.

			  CLOCK          (alpha = 0, k = 0)  evict immediately.
			  CA-CLOCK prob  (alpha > 0)         spare a dirty page with probability alpha.
			  CA-CLOCK bound (k > 0)             spare a dirty page at most k times.

			The bounded form exists because the probabilistic one has no upper limit
			on how long a dirty page can dodge eviction. Measured at alpha = 1.0 with
			a write-heavy workload, that produced 809,990 sparing decisions and left
			59% of evictions falling through to the give-up path below — at which
			point the policy is no longer CLOCK. A counter caps the damage: after k
			skips the page is evicted no matter what.
			*/
			if (entry.dirty)
			{
				if (max_dirty_skips > 0)
				{
					if (entry.dirty_skips < max_dirty_skips)
					{
						++entry.dirty_skips;
						++g_dirty_spare_count;
						continue;
					}
					// Budget spent. Evict, and let the next tenant start fresh.
				}
				else if (alpha > 0.0 && uniform_dist(g_rng) < alpha)
				{
					++g_dirty_spare_count;
					continue;
				}
			}

			// This frame is a victim.
			return frame;
		}

		// Safety: we scanned 2*capacity and found no victim, which means the dirty
		// branch spared everything. Evict whatever the hand landed on so the pager
		// always makes progress, and record that the policy had to give up.
		++g_scan_exhausted_count;
		return ring.hand;
	}

} // namespace

FrameRing create_frame_ring(int capacity)
{
	assert(capacity > 0);
	FrameRing ring;
	ring.page_of_frame.resize(static_cast<std::vector<int>::size_type>(capacity), -1);
	ring.hand = 0;
	ring.capacity = capacity;
	ring.resident_count = 0;
	return ring;
}

int policy_select_victim(FrameRing &ring, PageTable &page_table, unsigned char *arena_base,
						 double alpha, int max_dirty_skips, std::string &error_message)
{
	return select_victim_with_alpha(ring, page_table, arena_base, alpha, max_dirty_skips, error_message);
}

size_t policy_get_and_reset_dirty_spare_count()
{
	size_t value = g_dirty_spare_count;
	g_dirty_spare_count = 0;
	return value;
}

size_t policy_get_and_reset_scan_exhausted_count()
{
	size_t value = g_scan_exhausted_count;
	g_scan_exhausted_count = 0;
	return value;
}

void policy_seed_rng(unsigned seed)
{
	g_rng.seed(seed);
}

void policy_page_resident(FrameRing &ring, int page_index, int frame_index)
{
	assert(page_index >= 0);
	assert(frame_index >= 0 && frame_index < ring.capacity);
	assert(ring.page_of_frame[frame_index] == -1 || ring.page_of_frame[frame_index] == page_index);

	if (ring.page_of_frame[frame_index] == -1)
	{
		ring.resident_count++;
	}
	ring.page_of_frame[frame_index] = page_index;
}

void policy_page_evicted(FrameRing &ring, int page_index, int frame_index)
{
	assert(page_index >= 0);
	assert(frame_index >= 0 && frame_index < ring.capacity);
	assert(ring.page_of_frame[frame_index] == page_index);

	ring.page_of_frame[frame_index] = -1;
	ring.resident_count--;
	assert(ring.resident_count >= 0);
}
