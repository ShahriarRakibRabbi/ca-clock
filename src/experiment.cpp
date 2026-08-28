#include "experiment.h"
#include "store.h"
#include "arena.h"
#include "workload.h"
#include "stats.h"

#include <random>

namespace
{
	double wall_clock_us_since(LARGE_INTEGER start)
	{
		LARGE_INTEGER frequency;
		LARGE_INTEGER now;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&now);
		return static_cast<double>(now.QuadPart - start.QuadPart) * 1000000.0 /
			   static_cast<double>(frequency.QuadPart);
	}
} // namespace

bool run_experiment(const Config &config, Stats &stats, std::string &error_message)
{
	stats = create_stats();

	Store store = open_store(config.backing_path.empty() ? std::string("results/backing.dat") : config.backing_path,
							 config.total_pages, config.durable_writes, true /* reset for reproducibility */, error_message);
	if (store.handle == INVALID_HANDLE_VALUE || store.io_buffer == NULL)
	{
		return false;
	}

	// Creating the backing file can involve thousands of writes. Those are setup
	// cost, not experiment cost, so the latency counters start from zero here.
	store_reset_measurements(store);

	Arena arena = create_arena(config.total_pages, config.frame_budget, config.alpha,
							   config.max_dirty_skips, store, error_message);
	if (arena.base_address == NULL)
	{
		close_store(store);
		return false;
	}

	activate_arena(arena);

	Workload workload;
	if (config.pattern == "sequential")
	{
		workload = create_sequential_workload(config.total_pages, config.seed);
	}
	else
	{
		workload = create_zipf_workload(config.total_pages, config.zipf_s, config.seed);
	}

	// Two independent streams so that changing alpha cannot change which pages
	// are touched or which accesses are writes. Without this the policy and the
	// workload would share an RNG and every alpha would see a different trace,
	// making the comparison meaningless.
	std::mt19937 access_rng(config.seed);
	std::uniform_real_distribution<double> uniform(0.0, 1.0);
	policy_seed_rng(config.seed ^ 0x9E3779B9u);
	policy_get_and_reset_dirty_spare_count();
	policy_get_and_reset_scan_exhausted_count();

	volatile unsigned char *base = arena.base_address;
	unsigned long long checksum = 0;

	LARGE_INTEGER started;
	QueryPerformanceCounter(&started);

	bool ok = true;
	for (long i = 0; i < config.access_count; ++i)
	{
		const int page = workload_next(workload);
		const bool is_write = uniform(access_rng) < config.write_ratio;
		volatile unsigned char *address = base + static_cast<SIZE_T>(PAGE_BYTES) * page;

		if (is_write)
		{
			*address = static_cast<unsigned char>(i & 0xFF);
			++stats.write_accesses;
		}
		else
		{
			checksum += *address;
			++stats.read_accesses;
		}

		if (arena.fault_loop_detected)
		{
			error_message = "fault-loop guard triggered: the handler stopped making progress";
			ok = false;
			break;
		}

		if (config.dump_every > 0 && (i % config.dump_every) == 0)
		{
			dump_arena_state(arena);
		}

		if (config.delay_ms > 0)
		{
			Sleep(static_cast<DWORD>(config.delay_ms));
		}
	}

	// Flush pages that are still resident and dirty, the same way an OS writes
	// back dirty pages when a mapping is torn down.
	if (ok && !arena_flush_dirty_pages(arena, error_message))
	{
		ok = false;
	}

	stats.wall_clock_us = wall_clock_us_since(started);
	stats.checksum = checksum;

	stats.faults_total = static_cast<long>(arena.fault_count);
	stats.faults_pagein = arena.faults_pagein;
	stats.faults_write_protect = arena.faults_write_protect;
	stats.faults_reference = arena.faults_reference;
	stats.evictions = arena.eviction_count;
	stats.dirty_spares = static_cast<long>(policy_get_and_reset_dirty_spare_count());
	stats.scan_exhausted = static_cast<long>(policy_get_and_reset_scan_exhausted_count());

	stats.pagein_reads = store.read_count;
	stats.writeback_writes = store.write_count;
	stats.io_read_us_total = store.read_us_total;
	stats.io_write_us_total = store.write_us_total;
	stats.ewma_read_us = store.ewma_read_us;
	stats.ewma_write_us = store.ewma_write_us;

	destroy_workload(workload);
	deactivate_arena(arena);
	destroy_arena(arena);
	close_store(store);

	return ok;
}
