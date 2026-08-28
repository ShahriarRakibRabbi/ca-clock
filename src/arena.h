#pragma once

#include <string>

#include <windows.h>

#include "pagetable.h"
#include "policy.h"
#include "store.h"

struct Arena
{
	unsigned char *base_address;
	int total_pages;
	int frame_budget;
	PageTable page_table;
	FrameRing frame_ring;
	Store *store;
	PVOID vectored_handler;
	LONG last_fault_code;
	double alpha;		  // probabilistic dirty sparing, 0 = off
	int max_dirty_skips;  // bounded second chance, 0 = off; takes precedence over alpha

	/*
	Fault accounting. These are incremented inside the exception handler, which
	may not allocate or print, so every observation the project makes about
	demand paging has to arrive through one of these counters.
	  fault_count            every access violation we handled
	  faults_pagein          faults that required reading a page from the store
	  faults_write_protect   faults that turned a clean resident page dirty
	  faults_reference       faults that only re-set the reference bit
	  eviction_count         victims chosen by the CLOCK/CA-CLOCK hand
	  writeback_count        victims that were dirty and had to be written out
	*/
	unsigned long long fault_count;
	long faults_pagein;
	long faults_write_protect;
	long faults_reference;
	long eviction_count;
	long writeback_count;

	// Fault-loop guard state: a fault is only "repeated" if we already handled a
	// fault on this page and left it at the same protection, meaning we made no
	// progress and the next retry will fault identically.
	int last_faulted_page;
	DWORD last_applied_protection;
	int consecutive_faults_no_progress;
	bool fault_loop_detected;

	bool broken_handler_for_testing;
};
Arena create_arena(int total_pages, int frame_budget, double alpha, int max_dirty_skips,
				   Store &store, std::string &error_message);
void activate_arena(Arena &arena);
void deactivate_arena(Arena &arena);
void destroy_arena(Arena &arena);
bool touch_page_for_test(Arena &arena, int page_index, std::string &error_message);
void dump_arena_state(const Arena &arena);
void arena_set_broken_handler_for_testing(Arena &arena, bool broken);

// Write back every page that is still resident and dirty. Called at the end of a
// run so the backing file reflects all completed writes, exactly as an OS flushes
// dirty pages at unmap time. Without this the last generation of dirty pages
// would be silently lost and the eviction round-trip test could not pass.
bool arena_flush_dirty_pages(Arena &arena, std::string &error_message);
