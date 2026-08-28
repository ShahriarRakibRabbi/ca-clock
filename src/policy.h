#pragma once

#include <cstddef> // size_t
#include <string>
#include <vector>

#include <windows.h>

#include "pagetable.h"

/*
The frame ring is the circular buffer of physical frames that CLOCK sweeps through.
The hand position tracks where we are in the rotation. Each frame holds the index of
the virtual page it contains, or -1 if empty. This structure is separate from the
page table so that the policy logic does not need to iterate all virtual pages to
find victims — it only looks at resident frames.
*/
struct FrameRing
{
	std::vector<int> page_of_frame; // page_of_frame[f] = virtual page index, or -1 if empty
	int hand;						// current position in the ring
	int capacity;					// the frame budget enforced by MEM_DECOMMIT
	int resident_count;				// how many frames are in use (should be <= capacity)
};

FrameRing create_frame_ring(int capacity);

/*
Victim selection for all three policies. The hand advances around the ring,
clearing reference bits by demoting pages to PAGE_NOACCESS, until it finds a
frame it is willing to evict.

  alpha = 0, max_dirty_skips = 0   plain CLOCK
  alpha > 0                        CA-CLOCK, probabilistic dirty sparing
  max_dirty_skips = k > 0          CA-CLOCK, bounded second chance (takes
                                   precedence over alpha; k skips per page)

Returns the frame index to evict, or -1 on a Win32 failure. The caller performs
any write-back and the decommit; this function does no I/O.
*/
int policy_select_victim(FrameRing &ring, PageTable &page_table, unsigned char *arena_base,
						 double alpha, int max_dirty_skips, std::string &error_message);

// For testing: number of times CA-CLOCK spared a dirty page in the last run.
size_t policy_get_and_reset_dirty_spare_count();

// For testing: number of times the bounded scan found no eligible victim and had
// to evict the frame under the hand. A large value means alpha is high enough to
// degrade CA-CLOCK towards random replacement.
size_t policy_get_and_reset_scan_exhausted_count();

// Seed the internal RNG used by CA-CLOCK so experiments are repeatable.
void policy_seed_rng(unsigned seed);

/*
Marks a page as resident in the ring when it is paged in. The frame_index is where
the page table says the page lives; this updates the ring so we know which page is
in that frame.
*/
void policy_page_resident(FrameRing &ring, int page_index, int frame_index);

/*
Marks a page as evicted from the ring. The frame becomes empty and available.
*/
void policy_page_evicted(FrameRing &ring, int page_index, int frame_index);
