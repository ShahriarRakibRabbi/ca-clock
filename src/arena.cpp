#include "arena.h"

#include <cassert>
#include <iostream>
#include <sstream>

namespace
{
	Arena *g_active_arena = NULL;

	void set_error(std::string &error_message, const char *what, DWORD error_code)
	{
		std::ostringstream message;
		message << what << " failed with GetLastError()=" << error_code;
		error_message = message.str();
	}

	unsigned char *page_address(const Arena &arena, int page_index)
	{
		return arena.base_address + static_cast<SIZE_T>(PAGE_BYTES) * page_index;
	}

	/*
	A page is committed PAGE_READWRITE only for as long as it takes to copy the
	page in from the store; the caller immediately narrows it to READONLY or
	READWRITE according to the access that faulted. Committing READONLY here
	instead would make our own load_page_from_store memcpy fault, which the
	handler is in no position to service while it is already running.
	*/
	bool commit_page(Arena &arena, int page_index, std::string &error_message)
	{
		void *result = VirtualAlloc(page_address(arena, page_index), PAGE_BYTES, MEM_COMMIT, PAGE_READWRITE);
		if (result == NULL)
		{
			set_error(error_message, "VirtualAlloc", GetLastError());
			return false;
		}

		arena.page_table.entries[page_index].state = RESIDENT;
		arena.page_table.entries[page_index].referenced = true;
		// A freshly loaded page matches the store exactly, so it starts clean.
		// This is the ONLY place the dirty bit may be cleared: a page that is
		// already resident and dirty owes the store a write-back, and nothing
		// short of performing that write-back cancels the debt.
		arena.page_table.entries[page_index].dirty = false;
		arena.page_table.entries[page_index].dirty_skips = 0;
		// frame_index will be set by the caller when a frame slot is chosen
		return true;
	}

	bool set_protection(Arena &arena, int page_index, DWORD protection, std::string &error_message)
	{
		DWORD old_protection = 0;
		if (VirtualProtect(page_address(arena, page_index), PAGE_BYTES, protection, &old_protection) == 0)
		{
			set_error(error_message, "VirtualProtect", GetLastError());
			return false;
		}
		return true;
	}

	bool load_page_from_store(Arena &arena, int page_index, std::string &error_message)
	{
		return read_page(*arena.store, page_index, page_address(arena, page_index), error_message);
	}

	/*
	Evict the page in victim_frame. A dirty victim must reach the store before its
	memory is released — that write-back is the cost CA-CLOCK exists to avoid, so
	it is counted separately from ordinary evictions.
	*/
	bool evict_frame(Arena &arena, int victim_frame, std::string &error_message)
	{
		const int victim_page = arena.frame_ring.page_of_frame[victim_frame];
		if (victim_page < 0)
		{
			return true; // frame was already empty; nothing to write back
		}

		PageEntry &victim_entry = arena.page_table.entries[victim_page];
		if (victim_entry.dirty)
		{
			if (!write_page(*arena.store, victim_page, page_address(arena, victim_page), error_message))
			{
				return false;
			}
			++arena.writeback_count;
		}

		if (VirtualFree(page_address(arena, victim_page), PAGE_BYTES, MEM_DECOMMIT) == 0)
		{
			set_error(error_message, "VirtualFree", GetLastError());
			return false;
		}

		victim_entry.state = NOT_RESIDENT;
		victim_entry.referenced = false;
		victim_entry.dirty = false;
		victim_entry.frame_index = -1;
		victim_entry.dirty_skips = 0;
		policy_page_evicted(arena.frame_ring, victim_page, victim_frame);
		++arena.eviction_count;
		return true;
	}

	/*
	The three-state protection machine, and the one place the software reference
	and dirty bits are maintained.

	  PAGE_NOACCESS   resident but the clock hand has cleared R. Any access
	                  faults, which is how we learn the page was referenced.
	  PAGE_READONLY   referenced and clean. A WRITE faults, which is how we
	                  learn the page became dirty.
	  PAGE_READWRITE  referenced and dirty. Nothing left to observe, so no fault.

	The subtle case is a page that is already dirty and gets demoted to NOACCESS
	by the hand, then *read*. The read tells us R should be set again, but it says
	nothing about D — the page still holds modifications the store has never seen.
	Restoring READONLY there would be worse than useless: the next write would
	fault, and the fault would be the only thing keeping D alive. So a dirty page
	always comes back as READWRITE, and D is never cleared outside commit_page.
	*/
	DWORD protection_after_fault(const PageEntry &entry)
	{
		return entry.dirty ? PAGE_READWRITE : PAGE_READONLY;
	}

	bool handle_page_fault(Arena &arena, int page_index, bool is_write, DWORD &applied_protection, std::string &error_message)
	{
		assert(page_index >= 0);
		assert(page_index < arena.total_pages);

		// Test hook: deliberately fail to test fault-loop guard.
		if (arena.broken_handler_for_testing)
		{
			error_message = "handler deliberately broken for testing";
			return false;
		}

		PageEntry &entry = arena.page_table.entries[page_index];
		const bool needed_pagein = (entry.state == NOT_RESIDENT);
		if (needed_pagein)
		{
			int assigned_frame = -1;
			if (arena.frame_ring.resident_count >= arena.frame_budget)
			{
				const int victim_frame = policy_select_victim(arena.frame_ring, arena.page_table, arena.base_address,
															  arena.alpha, arena.max_dirty_skips, error_message);
				if (victim_frame < 0)
				{
					if (error_message.empty())
					{
						error_message = "policy_select_victim failed";
					}
					return false;
				}

				if (!evict_frame(arena, victim_frame, error_message))
				{
					return false;
				}
				assigned_frame = victim_frame;
			}
			else
			{
				for (int f = 0; f < arena.frame_ring.capacity; ++f)
				{
					if (arena.frame_ring.page_of_frame[f] == -1)
					{
						assigned_frame = f;
						break;
					}
				}
			}

			if (assigned_frame < 0)
			{
				error_message = "no free frame available for page-in";
				return false;
			}

			if (!commit_page(arena, page_index, error_message))
			{
				return false;
			}

			entry.frame_index = assigned_frame;
			policy_page_resident(arena.frame_ring, page_index, assigned_frame);

			if (!load_page_from_store(arena, page_index, error_message))
			{
				return false;
			}
		}

		// Classify the fault before mutating the bits, so the counters describe
		// what the CPU actually asked for rather than the state we leave behind.
		// The three categories are mutually exclusive and sum to fault_count.
		if (needed_pagein)
		{
			++arena.faults_pagein;
		}
		else if (is_write && !entry.dirty)
		{
			++arena.faults_write_protect; // clean -> dirty transition
		}
		else
		{
			++arena.faults_reference; // page was at PAGE_NOACCESS; R is set again
		}

		entry.referenced = true;
		// The page was touched again, so it has earned a fresh skip budget. This
		// is what stops bounded-k from evicting a page that is genuinely hot just
		// because it happened to be passed k times earlier in the run.
		entry.dirty_skips = 0;
		if (is_write)
		{
			entry.dirty = true;
		}

		applied_protection = protection_after_fault(entry);
		return set_protection(arena, page_index, applied_protection, error_message);
	}

	/*
	The vectored exception handler is the critical path for making progress on page
	faults. If handle_page_fault returns false, the handler cannot recover from the
	fault. If the same page faults repeatedly without resolving, it indicates a
	broken handler or corrupted state. This guard detects such fault loops and aborts
	rather than entering an infinite loop, making it safe to add guards within
	handle_page_fault itself.
	*/
	LONG CALLBACK arena_vectored_exception_handler(EXCEPTION_POINTERS *info)
	{
		if (g_active_arena == NULL || info == NULL || info->ExceptionRecord == NULL)
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		const ULONG_PTR fault_address_value = info->ExceptionRecord->ExceptionInformation[1];
		unsigned char *fault_address = reinterpret_cast<unsigned char *>(fault_address_value);
		if (fault_address < g_active_arena->base_address)
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		const SIZE_T offset = static_cast<SIZE_T>(fault_address - g_active_arena->base_address);
		const SIZE_T page_index_size = offset / PAGE_BYTES;
		if (page_index_size >= static_cast<SIZE_T>(g_active_arena->total_pages))
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		const int page_index = static_cast<int>(page_index_size);

		const bool is_write = info->ExceptionRecord->ExceptionInformation[0] == 1;
		DWORD applied_protection = 0;
		std::string error_message;
		if (!handle_page_fault(*g_active_arena, page_index, is_write, applied_protection, error_message))
		{
			g_active_arena->last_fault_code = static_cast<LONG>(GetLastError());
			return EXCEPTION_CONTINUE_SEARCH;
		}

		/*
		Fault-loop guard. Counting consecutive faults on the same page is not
		enough on its own: a legitimate sequence (NOACCESS -> read -> READONLY ->
		write -> READWRITE) faults twice on one page and is perfectly healthy.
		What is never healthy is faulting on a page we just left at the SAME
		protection, because the retried instruction will fault identically for
		ever. That is the condition we count.
		*/
		if (page_index == g_active_arena->last_faulted_page &&
			applied_protection == g_active_arena->last_applied_protection)
		{
			++g_active_arena->consecutive_faults_no_progress;
		}
		else
		{
			g_active_arena->consecutive_faults_no_progress = 1;
		}
		g_active_arena->last_faulted_page = page_index;
		g_active_arena->last_applied_protection = applied_protection;

		const int MAX_FAULTS_WITHOUT_PROGRESS = 3;
		if (g_active_arena->consecutive_faults_no_progress > MAX_FAULTS_WITHOUT_PROGRESS)
		{
			// Cannot print from inside the handler; raise a flag the main loop reads.
			g_active_arena->fault_loop_detected = true;
			g_active_arena->last_fault_code = -1;
			return EXCEPTION_CONTINUE_SEARCH; // let it crash rather than hang
		}

		++g_active_arena->fault_count;
		return EXCEPTION_CONTINUE_EXECUTION;
	}

} // namespace

/*
The arena is the virtual memory region that the CPU will actually fault on.
Each resident page has a protection mode that stands in for the software
reference and dirty bits. The first access fault is what teaches us whether the
page was read or written, and the handler must return only after the access is
safe to retry. The frame budget is enforced by decommitting pages when we reach
capacity and need to page in a new one.
The vector handler is intentionally small because later policy code should be
able to reuse the same memory layout and store logic without changing how the
CPU faults are recovered.
*/
Arena create_arena(int total_pages, int frame_budget, double alpha, int max_dirty_skips,
				   Store &store, std::string &error_message)
{
	assert(total_pages > 0);
	assert(frame_budget > 0 && frame_budget <= total_pages);
	assert(max_dirty_skips >= 0);
	Arena arena;
	arena.base_address = static_cast<unsigned char *>(VirtualAlloc(NULL, static_cast<SIZE_T>(total_pages) * PAGE_BYTES, MEM_RESERVE, PAGE_NOACCESS));
	if (arena.base_address == NULL)
	{
		set_error(error_message, "VirtualAlloc", GetLastError());
		return arena;
	}

	arena.total_pages = total_pages;
	arena.frame_budget = frame_budget;
	arena.alpha = alpha;
	arena.max_dirty_skips = max_dirty_skips;
	arena.page_table = create_pagetable(total_pages);
	arena.frame_ring = create_frame_ring(frame_budget);
	arena.store = &store;
	arena.vectored_handler = AddVectoredExceptionHandler(1, arena_vectored_exception_handler);
	if (arena.vectored_handler == NULL)
	{
		set_error(error_message, "AddVectoredExceptionHandler", GetLastError());
		VirtualFree(arena.base_address, 0, MEM_RELEASE);
		arena.base_address = NULL;
		return arena;
	}
	arena.last_fault_code = 0;
	arena.fault_count = 0;
	arena.faults_pagein = 0;
	arena.faults_write_protect = 0;
	arena.faults_reference = 0;
	arena.eviction_count = 0;
	arena.writeback_count = 0;
	arena.last_faulted_page = -1;
	arena.last_applied_protection = 0;
	arena.consecutive_faults_no_progress = 0;
	arena.fault_loop_detected = false;
	arena.broken_handler_for_testing = false;
	return arena;
}

/*
At the end of a run some pages are still resident and dirty. A real OS writes
those back when the mapping is torn down; if we did not, the last generation of
writes would never reach the file and a read-back check would fail on data that
was in fact stored correctly. These write-backs are counted like any other,
because they are real device writes that the policy caused.
*/
bool arena_flush_dirty_pages(Arena &arena, std::string &error_message)
{
	for (int page_index = 0; page_index < arena.total_pages; ++page_index)
	{
		PageEntry &entry = arena.page_table.entries[page_index];
		if (entry.state != RESIDENT || !entry.dirty)
		{
			continue;
		}

		// The page may be sitting at PAGE_NOACCESS after a hand sweep, so make it
		// readable before we copy it out.
		if (!set_protection(arena, page_index, PAGE_READONLY, error_message))
		{
			return false;
		}
		if (!write_page(*arena.store, page_index, page_address(arena, page_index), error_message))
		{
			return false;
		}
		++arena.writeback_count;
		entry.dirty = false;
	}
	return true;
}

void activate_arena(Arena &arena)
{
	g_active_arena = &arena;
}

void deactivate_arena(Arena &arena)
{
	if (g_active_arena == &arena)
	{
		g_active_arena = NULL;
	}
}

void destroy_arena(Arena &arena)
{
	if (arena.vectored_handler != NULL)
	{
		RemoveVectoredExceptionHandler(arena.vectored_handler);
		arena.vectored_handler = NULL;
	}

	deactivate_arena(arena);

	if (arena.base_address != NULL)
	{
		VirtualFree(arena.base_address, 0, MEM_RELEASE);
		arena.base_address = NULL;
	}
}

/*
This helper exists only for the milestone test. It performs one volatile write
into the arena so the CPU must raise a real access violation. The test then
checks that the handler repaired the page and recorded exactly one fault. If we
can make one touch recover cleanly, the later workload can use the same path for
all page-ins and dirty updates.
*/
bool touch_page_for_test(Arena &arena, int page_index, std::string &error_message)
{
	if (page_index < 0 || page_index >= arena.total_pages)
	{
		error_message = "touch_page_for_test received an out-of-range page index";
		return false;
	}

	volatile unsigned char *address = arena.base_address + static_cast<int>(PAGE_BYTES) * page_index;
	*address = static_cast<unsigned char>(page_index & 0xFF);
	if (arena.page_table.entries[page_index].state != RESIDENT)
	{
		error_message = "page fault handler did not commit the page";
		return false;
	}
	if (arena.fault_count == 0)
	{
		error_message = "page fault handler did not record a fault";
		return false;
	}
	return true;
}

/*
The demo view. One column per physical frame, not per virtual page, because the
frame ring is what CLOCK actually sweeps and what the frame budget constrains.
  .  empty      r  resident clean      D  resident dirty      ^  the clock hand
Seeing the hand walk along the row and flip D back to . is the clearest possible
picture of what the policy does, which is why the video config turns this on.
*/
void dump_arena_state(const Arena &arena)
{
	std::string frames;
	std::string hand;
	for (int f = 0; f < arena.frame_ring.capacity; ++f)
	{
		const int page_index = arena.frame_ring.page_of_frame[f];
		if (page_index < 0)
		{
			frames += '.';
		}
		else
		{
			frames += arena.page_table.entries[page_index].dirty ? 'D' : 'r';
		}
		hand += (f == arena.frame_ring.hand) ? '^' : ' ';
	}

	std::cout << "frames [" << frames << "]  resident " << arena.frame_ring.resident_count
			  << "/" << arena.frame_budget
			  << "  faults " << arena.fault_count
			  << "  page-ins " << arena.faults_pagein
			  << "  write-backs " << arena.writeback_count << '\n';
	std::cout << "        " << hand << '\n';
}

void arena_set_broken_handler_for_testing(Arena &arena, bool broken)
{
	arena.broken_handler_for_testing = broken;
}