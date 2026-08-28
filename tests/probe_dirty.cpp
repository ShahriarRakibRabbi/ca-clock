/*
The probe that found the data-loss bug.

It asks one question: if a page is written, then demoted to PAGE_NOACCESS by the
clock hand, then merely READ, does the pager still remember it owes the store a
write-back?

The page layout is chosen deliberately. Pages 1-3 are loaded first so that page 0
lands in the LAST frame of the ring, which makes it survive the first eviction
sweep while still being demoted to PAGE_NOACCESS by the hand. That is the exact
state the bug needed, and it does not occur if page 0 is touched first.

To SEE the bug rather than the fix, open src/arena.cpp, find protection_after_fault
and make it always return PAGE_READONLY, then also add `entry.dirty = false;` to
the read path in handle_page_fault. Rebuild and run this again: step 3 reports
dirty=0 and step 4 finds the byte missing from disk. Put it back afterwards.

Build:  see build.bat  (produces probe.exe)
Run:    probe.exe
*/
#include "arena.h"
#include "store.h"

#include <iostream>
#include <string>
#include <vector>

int main()
{
	const char *path = "results/probe_backing.dat";
	std::string error;

	Store store = open_store(path, 16, false, true, error);
	if (store.handle == INVALID_HANDLE_VALUE)
	{
		std::cerr << "open_store failed: " << error << '\n';
		return 2;
	}

	Arena arena = create_arena(16, 4, 0.0, 0, store, error);
	if (arena.base_address == NULL)
	{
		std::cerr << "create_arena failed: " << error << '\n';
		close_store(store);
		return 2;
	}
	activate_arena(arena);

	volatile unsigned char *base = arena.base_address;

	std::cout << "Probe: does a read fault erase a page's dirty bit?\n\n";

	// Fill frames 0..2 with pages 1,2,3 so that page 0 lands in frame 3.
	for (int page = 1; page <= 3; ++page)
	{
		volatile unsigned char value = base[static_cast<SIZE_T>(PAGE_BYTES) * page];
		(void)value;
	}

	base[0] = 0xAA;
	std::cout << "1. wrote 0xAA to page 0        dirty=" << arena.page_table.entries[0].dirty
			  << "  frame=" << arena.page_table.entries[0].frame_index << '\n';

	// Touch page 4. The ring is full, so this runs a sweep: the hand clears every
	// reference bit (demoting page 0 to PAGE_NOACCESS) and evicts page 1.
	{
		volatile unsigned char value = base[static_cast<SIZE_T>(PAGE_BYTES) * 4];
		(void)value;
	}
	std::cout << "2. after eviction sweep        page 0 is "
			  << (arena.page_table.entries[0].state == RESIDENT ? "RESIDENT" : "NOT_RESIDENT")
			  << "  dirty=" << arena.page_table.entries[0].dirty << '\n';

	int failures = 0;
	if (arena.page_table.entries[0].state != RESIDENT)
	{
		std::cout << "\n   page 0 was evicted early; probe inconclusive\n";
		failures = 1;
	}
	else
	{
		// Page 0 is resident but PAGE_NOACCESS, so this READ faults.
		volatile unsigned char value = base[0];
		const bool still_dirty = arena.page_table.entries[0].dirty;
		std::cout << "3. after a READ fault on it    dirty=" << still_dirty
				  << (still_dirty ? "  (correct: the write is still owed to disk)"
								  : "  <-- BUG: the pager forgot the write")
				  << '\n';
		std::cout << "   byte still in memory        0x" << std::hex << (int)value << std::dec << '\n';
		if (!still_dirty)
		{
			++failures;
		}

		// Push page 0 out for real, then look at the file itself.
		for (int page = 5; page <= 12; ++page)
		{
			volatile unsigned char v = base[static_cast<SIZE_T>(PAGE_BYTES) * page];
			(void)v;
		}
		if (!arena_flush_dirty_pages(arena, error))
		{
			std::cerr << "flush failed: " << error << '\n';
		}

		std::vector<unsigned char> page_buffer(PAGE_BYTES);
		if (read_page(store, 0, &page_buffer[0], error))
		{
			const bool survived = (page_buffer[0] == 0xAA);
			std::cout << "4. byte 0 as stored on disk    0x" << std::hex << (int)page_buffer[0] << std::dec
					  << (survived ? "  (correct: the write reached the device)"
								   : "  <-- DATA LOSS: the write was discarded")
					  << '\n';
			if (!survived)
			{
				++failures;
			}
		}
	}

	std::cout << "\nfaults=" << arena.fault_count
			  << "  evictions=" << arena.eviction_count
			  << "  write-backs=" << arena.writeback_count << '\n';

	deactivate_arena(arena);
	destroy_arena(arena);
	close_store(store);
	DeleteFileA("results\\probe_backing.dat");

	std::cout << (failures == 0 ? "\nPROBE PASSED: no data was lost.\n"
							    : "\nPROBE FAILED: the pager is losing writes.\n");
	return failures == 0 ? 0 : 1;
}
