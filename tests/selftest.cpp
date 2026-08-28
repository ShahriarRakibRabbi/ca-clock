/*
Section F self-tests.

Each test is a claim about the pager that could plausibly be wrong, written so
that a failure prints what was expected and what happened. Tests 1-4 are about
correctness (does the pager lose data?), 5-8 are about the research result (does
alpha do what the project says it does?).

Build:
  g++ -std=c++17 -O2 -I src tests\selftest.cpp src\config.cpp src\store.cpp
      src\pagetable.cpp src\arena.cpp src\policy.cpp src\workload.cpp
      src\stats.cpp src\experiment.cpp -o selftest.exe -lpsapi
*/
#include "arena.h"
#include "config.h"
#include "experiment.h"
#include "pagetable.h"
#include "policy.h"
#include "stats.h"
#include "store.h"
#include "workload.h"

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;

	void check(bool condition, const std::string &what)
	{
		std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
		if (!condition)
		{
			++g_failures;
		}
	}

	Config test_config(int total_pages, int frame_budget, long accesses, double alpha, double write_ratio)
	{
		Config config = default_config();
		config.total_pages = total_pages;
		config.frame_budget = frame_budget;
		config.access_count = accesses;
		config.alpha = alpha;
		config.write_ratio = write_ratio;
		config.pattern = "zipf";
		config.zipf_s = 0.99;
		config.durable_writes = false;
		config.seed = 12345;
		config.backing_path = "results/selftest_backing.dat";
		return config;
	}

	// ---- F1: store round trip -------------------------------------------------
	// Catches every alignment and offset bug in the unbuffered I/O path.
	void test_store_round_trip()
	{
		std::cout << "F1 store round trip\n";
		std::string error;
		const std::string path = "results/selftest_roundtrip.dat";

		Store store = open_store(path, 16, false, true, error);
		if (store.handle == INVALID_HANDLE_VALUE)
		{
			check(false, "open_store: " + error);
			return;
		}

		std::vector<unsigned char> written(PAGE_BYTES);
		std::vector<unsigned char> read_back(PAGE_BYTES);
		for (long i = 0; i < PAGE_BYTES; ++i)
		{
			written[static_cast<size_t>(i)] = static_cast<unsigned char>((i * 7) & 0xFF);
		}

		check(write_page(store, 5, &written[0], error), "write page 5: " + error);
		close_store(store);

		// reset_contents = false: we want to see what is actually on disk.
		Store reopened = open_store(path, 16, false, false, error);
		if (reopened.handle == INVALID_HANDLE_VALUE)
		{
			check(false, "reopen_store: " + error);
			return;
		}
		check(read_page(reopened, 5, &read_back[0], error), "read page 5: " + error);
		close_store(reopened);

		check(std::memcmp(&written[0], &read_back[0], PAGE_BYTES) == 0,
			  "page 5 survived close and reopen byte for byte");
		DeleteFileA(path.c_str());
	}

	// ---- F2: a single touch produces exactly one real fault -------------------
	void test_single_fault_recovery()
	{
		std::cout << "F2 single fault recovery\n";
		std::string error;
		Store store = open_store("results/selftest_backing.dat", 16, false, true, error);
		if (store.handle == INVALID_HANDLE_VALUE)
		{
			check(false, "open_store: " + error);
			return;
		}

		Arena arena = create_arena(16, 4, 0.0, 0, store, error);
		if (arena.base_address == NULL)
		{
			check(false, "create_arena: " + error);
			close_store(store);
			return;
		}
		activate_arena(arena);

		check(touch_page_for_test(arena, 3, error), "one touch recovered: " + error);
		check(arena.fault_count == 1, "exactly one fault was handled");
		check(arena.frame_ring.resident_count == 1, "exactly one frame is resident");
		check(arena.page_table.entries[3].dirty, "the touch was a write, so page 3 is dirty");

		deactivate_arena(arena);
		destroy_arena(arena);
		close_store(store);
	}

	// ---- F3: the frame budget is never exceeded -------------------------------
	void test_budget_enforcement()
	{
		std::cout << "F3 budget enforcement\n";
		std::string error;
		const int budget = 16;
		Store store = open_store("results/selftest_backing.dat", 100, false, true, error);
		if (store.handle == INVALID_HANDLE_VALUE)
		{
			check(false, "open_store: " + error);
			return;
		}

		Arena arena = create_arena(100, budget, 0.0, 0, store, error);
		if (arena.base_address == NULL)
		{
			check(false, "create_arena: " + error);
			close_store(store);
			return;
		}
		activate_arena(arena);

		volatile unsigned char *base = arena.base_address;
		bool never_exceeded = true;
		for (int page = 0; page < 100; ++page)
		{
			volatile unsigned char value = base[static_cast<SIZE_T>(PAGE_BYTES) * page];
			(void)value;
			if (arena.frame_ring.resident_count > budget)
			{
				never_exceeded = false;
				break;
			}
		}
		check(never_exceeded, "resident_count stayed <= 16 across 100 distinct pages");
		check(arena.frame_ring.resident_count == budget, "the budget ended up fully used");

		deactivate_arena(arena);
		destroy_arena(arena);
		close_store(store);
	}

	// ---- F4: eviction round trip (the data-loss test) -------------------------
	// Write a distinct byte to every page, force them all out, read them all back.
	// This is the test that catches a pager which silently drops modifications.
	void test_eviction_round_trip()
	{
		std::cout << "F4 eviction round trip (data loss)\n";
		std::string error;
		const int pages = 100;
		Store store = open_store("results/selftest_backing.dat", pages, false, true, error);
		if (store.handle == INVALID_HANDLE_VALUE)
		{
			check(false, "open_store: " + error);
			return;
		}

		Arena arena = create_arena(pages, 16, 0.0, 0, store, error);
		if (arena.base_address == NULL)
		{
			check(false, "create_arena: " + error);
			close_store(store);
			return;
		}
		activate_arena(arena);

		volatile unsigned char *base = arena.base_address;

		// Pass 1: write a signature byte into every page.
		for (int page = 0; page < pages; ++page)
		{
			base[static_cast<SIZE_T>(PAGE_BYTES) * page] = static_cast<unsigned char>((page * 3 + 11) & 0xFF);
		}

		// Pass 2: read every page back through the pager. Most have been evicted
		// and reloaded at least once by now.
		int mismatches_in_memory = 0;
		for (int page = 0; page < pages; ++page)
		{
			const unsigned char expected = static_cast<unsigned char>((page * 3 + 11) & 0xFF);
			if (base[static_cast<SIZE_T>(PAGE_BYTES) * page] != expected)
			{
				++mismatches_in_memory;
			}
		}
		check(mismatches_in_memory == 0, "every page still reads back its own signature through the pager");

		check(arena_flush_dirty_pages(arena, error), "final flush of resident dirty pages: " + error);

		deactivate_arena(arena);
		destroy_arena(arena);

		// Pass 3: bypass the pager entirely and check the file itself.
		int mismatches_on_disk = 0;
		std::vector<unsigned char> page_buffer(PAGE_BYTES);
		for (int page = 0; page < pages; ++page)
		{
			if (!read_page(store, page, &page_buffer[0], error))
			{
				++mismatches_on_disk;
				continue;
			}
			if (page_buffer[0] != static_cast<unsigned char>((page * 3 + 11) & 0xFF))
			{
				++mismatches_on_disk;
			}
		}
		check(mismatches_on_disk == 0, "every write reached the backing file (0 lost pages)");
		close_store(store);
	}

	// ---- F5: a read-only workload must issue exactly zero writes --------------
	void test_read_only_writes_nothing()
	{
		std::cout << "F5 read-only workload issues no writes\n";
		Config config = test_config(256, 16, 3000, 0.0, 0.0);
		config.csv_path = "results/selftest_runs.csv";
		config.label = "f5";

		Stats stats;
		std::string error;
		check(run_experiment(config, stats, error), "experiment ran: " + error);
		check(stats.write_accesses == 0, "the workload performed no write accesses");
		check(stats.writeback_writes == 0, "exactly zero pages were written to the store");
		check(stats.faults_pagein > 0, "reads still caused real page-ins");
	}

	// ---- F6: alpha = 0 is exactly CLOCK, and is deterministic -----------------
	void test_alpha_zero_is_clock()
	{
		std::cout << "F6 alpha = 0 is plain CLOCK\n";
		Config config = test_config(256, 16, 5000, 0.0, 0.4);
		config.csv_path = "results/selftest_runs.csv";
		config.label = "f6";

		Stats first;
		Stats second;
		std::string error;
		check(run_experiment(config, first, error), "first run: " + error);
		check(run_experiment(config, second, error), "second run: " + error);

		check(first.faults_total == second.faults_total &&
				  first.faults_pagein == second.faults_pagein &&
				  first.writeback_writes == second.writeback_writes &&
				  first.checksum == second.checksum,
			  "two identical runs produced identical counters and checksum");
		check(first.dirty_spares == 0 && second.dirty_spares == 0,
			  "the dirty-sparing branch was never taken at alpha = 0");
	}

	// ---- F7: alpha > 0 reduces write-backs ------------------------------------
	// The project's central claim, in test form. It asserts the DIRECTION of the
	// change, never a percentage, because the size of the effect depends on the
	// machine and the workload.
	//
	// Note what is NOT asserted: that page-ins go up. On a skewed workload they
	// often go DOWN as well, because a page that is dirty is usually a page that
	// is hot, so sparing dirty pages also happens to spare the working set. That
	// is a finding, not a bug, and it is reported rather than assumed.
	void test_alpha_reduces_writes(const char *pattern, double alpha, bool expect_reduction)
	{
		std::string error;

		Config baseline = test_config(256, 16, 20000, 0.0, 0.4);
		baseline.pattern = pattern;
		baseline.csv_path = "results/selftest_runs.csv";
		baseline.label = "f7-clock";

		Config proposed = baseline;
		proposed.alpha = alpha;
		proposed.label = "f7-caclock";

		Stats clock_stats;
		Stats caclock_stats;
		check(run_experiment(baseline, clock_stats, error), std::string("CLOCK run on ") + pattern + ": " + error);
		check(run_experiment(proposed, caclock_stats, error), std::string("CA-CLOCK run on ") + pattern + ": " + error);
		const long spares = caclock_stats.dirty_spares;
		const long exhausted = caclock_stats.scan_exhausted;

		std::cout << "       CLOCK      write-backs " << clock_stats.writeback_writes
				  << "  page-ins " << clock_stats.faults_pagein
				  << "  est I/O " << (stats_estimated_io_us(clock_stats) / 1000.0) << " ms\n";
		std::cout << "       CA-CLOCK   write-backs " << caclock_stats.writeback_writes
				  << "  page-ins " << caclock_stats.faults_pagein
				  << "  est I/O " << (stats_estimated_io_us(caclock_stats) / 1000.0) << " ms\n";
		std::cout << "       spared " << spares << "  scan-exhausted " << exhausted
				  << " of " << caclock_stats.evictions << " evictions\n";

		check(spares > 0, "CA-CLOCK actually spared dirty pages");

		if (expect_reduction)
		{
			check(caclock_stats.writeback_writes < clock_stats.writeback_writes,
				  "CA-CLOCK issued fewer write-backs than CLOCK");
		}
		else
		{
			// Sequential access never revisits a page before it is evicted, so every
			// frame is equally cold and equally likely to be dirty. Skipping one
			// dirty page just hands the eviction to another dirty page. CA-CLOCK
			// therefore cannot help here, and must not hurt either — the sparing
			// has to stay bounded rather than spiralling into extra evictions.
			const double baseline_writes = static_cast<double>(clock_stats.writeback_writes);
			const double proposed_writes = static_cast<double>(caclock_stats.writeback_writes);
			check(proposed_writes <= baseline_writes * 1.02,
				  "without reuse, CA-CLOCK is no worse than CLOCK (it cannot help)");
			check(exhausted == 0, "the bounded scan never had to give up");
		}
	}

	// ---- F9: bounded-k gets the write reduction without the collapse ----------
	// The reason bounded-k exists. At alpha = 1.0 on a write-heavy workload the
	// probabilistic policy spares a dirty page an unbounded number of times, the
	// scan runs out of sweeps, and the hand evicts an arbitrary frame - at which
	// point it is no longer CLOCK. A per-page skip budget makes that impossible by
	// construction, so scan_exhausted must be exactly zero, not merely small.
	void test_bounded_beats_unbounded()
	{
		std::cout << "F9 bounded-k avoids the alpha=1 collapse\n";
		std::string error;

		Config base = test_config(1024, 64, 20000, 0.0, 0.8);
		base.csv_path = "results/selftest_runs.csv";

		Config clock_config = base;
		clock_config.label = "f9-clock";

		Config unbounded = base;
		unbounded.alpha = 1.0;
		unbounded.label = "f9-prob";

		Config bounded = base;
		bounded.max_dirty_skips = 2;
		bounded.label = "f9-bounded";

		Stats clock_stats;
		Stats prob_stats;
		Stats bounded_stats;
		check(run_experiment(clock_config, clock_stats, error), "CLOCK run: " + error);
		check(run_experiment(unbounded, prob_stats, error), "alpha=1.0 run: " + error);
		check(run_experiment(bounded, bounded_stats, error), "k=2 run: " + error);

		std::cout << "       CLOCK       write-backs " << clock_stats.writeback_writes
				  << "  page-ins " << clock_stats.faults_pagein
				  << "  spares " << clock_stats.dirty_spares
				  << "  exhausted " << clock_stats.scan_exhausted << '\n';
		std::cout << "       alpha=1.00  write-backs " << prob_stats.writeback_writes
				  << "  page-ins " << prob_stats.faults_pagein
				  << "  spares " << prob_stats.dirty_spares
				  << "  exhausted " << prob_stats.scan_exhausted << '\n';
		std::cout << "       k=2         write-backs " << bounded_stats.writeback_writes
				  << "  page-ins " << bounded_stats.faults_pagein
				  << "  spares " << bounded_stats.dirty_spares
				  << "  exhausted " << bounded_stats.scan_exhausted << '\n';

		check(prob_stats.scan_exhausted > 0,
			  "alpha=1.0 does collapse here (this is the problem bounded-k solves)");
		check(bounded_stats.scan_exhausted == 0,
			  "bounded-k NEVER runs out of victims - guaranteed, not just unlikely");
		check(bounded_stats.dirty_spares < prob_stats.dirty_spares,
			  "bounded-k makes far fewer sparing decisions than unbounded alpha");
		check(bounded_stats.writeback_writes < clock_stats.writeback_writes,
			  "bounded-k still reduces write-backs against CLOCK");
	}

	// ---- F8: the fault-loop guard stops a broken handler ----------------------
	// Runs in a child process, because a handler that cannot make progress ends
	// with an unhandled access violation by design.
	void test_fault_loop_guard(const char *self_path)
	{
		std::cout << "F8 fault-loop guard\n";
		std::string command = "\"";
		command += self_path;
		command += "\" --break-handler";

		STARTUPINFOA startup;
		PROCESS_INFORMATION process;
		ZeroMemory(&startup, sizeof(startup));
		startup.cb = sizeof(startup);
		ZeroMemory(&process, sizeof(process));

		std::vector<char> mutable_command(command.begin(), command.end());
		mutable_command.push_back('\0');

		if (CreateProcessA(NULL, &mutable_command[0], NULL, NULL, FALSE,
						   CREATE_NO_WINDOW, NULL, NULL, &startup, &process) == 0)
		{
			check(false, "could not launch the child process for the guard test");
			return;
		}

		const DWORD started = GetTickCount();
		const DWORD wait_result = WaitForSingleObject(process.hProcess, 15000);
		const DWORD elapsed_ms = GetTickCount() - started;

		if (wait_result == WAIT_TIMEOUT)
		{
			TerminateProcess(process.hProcess, 1);
			check(false, "a broken handler HUNG (still alive after 15 s) instead of giving up");
		}
		else
		{
			DWORD exit_code = 0;
			GetExitCodeProcess(process.hProcess, &exit_code);
			std::ostringstream what;
			what << "a broken handler gave up after " << elapsed_ms
				 << " ms instead of looping forever (exit code " << exit_code << ")";
			// A clean exit would mean the broken handler somehow succeeded, which
			// would make the whole test meaningless. Non-zero is the pass.
			check(exit_code != 0, what.str());
		}
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);
	}

	/*
	The child half of F8.

	The exception this child provokes is genuinely unhandled, which normally hands
	the process to Windows Error Reporting. Measured on this machine, WER takes
	about 20 seconds to tear the process down, and none of that time has anything
	to do with what the test is checking. So the child suppresses WER and kills
	itself the instant the exception reaches the unhandled filter.

	This does not weaken the test. If the fault handler ever DID loop forever, no
	exception would reach this filter at all, nothing would call TerminateProcess,
	and the parent's timeout would still catch the hang.
	*/
	LONG CALLBACK die_without_error_reporting(EXCEPTION_POINTERS *info)
	{
		(void)info;
		TerminateProcess(GetCurrentProcess(), 3);
		return EXCEPTION_EXECUTE_HANDLER; // not reached
	}

	int run_broken_handler_child()
	{
		SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
		SetUnhandledExceptionFilter(die_without_error_reporting);

		std::string error;
		Store store = open_store("results/selftest_broken.dat", 16, false, true, error);
		if (store.handle == INVALID_HANDLE_VALUE)
		{
			return 2;
		}
		Arena arena = create_arena(16, 4, 0.0, 0, store, error);
		if (arena.base_address == NULL)
		{
			return 2;
		}
		activate_arena(arena);
		arena_set_broken_handler_for_testing(arena, true);

		// This access can never be serviced; the process must die, not hang.
		volatile unsigned char *base = arena.base_address;
		base[0] = 1;

		return 0; // unreachable if the guard behaves
	}

	void cleanup()
	{
		DeleteFileA("results\\selftest_backing.dat");
		DeleteFileA("results\\selftest_broken.dat");
		DeleteFileA("results\\selftest_runs.csv");
	}

} // namespace

int main(int argc, char **argv)
{
	if (argc > 1 && std::string(argv[1]) == "--break-handler")
	{
		return run_broken_handler_child();
	}

	// --quick runs only the correctness tests. F7 and F9 each run several
	// 20,000-access experiments against real unbuffered I/O, which is most of the
	// suite's runtime; skipping them is useful while iterating on the pager, and
	// for demonstrating the tests live. Never use --quick to judge a policy change.
	const bool quick = (argc > 1 && std::string(argv[1]) == "--quick");

	std::cout << "CA-CLOCK self-tests" << (quick ? "  [--quick: correctness only]" : "") << "\n\n";

	test_store_round_trip();
	test_single_fault_recovery();
	test_budget_enforcement();
	test_eviction_round_trip();
	test_read_only_writes_nothing();
	test_alpha_zero_is_clock();

	if (quick)
	{
		std::cout << "F7, F9 skipped (policy comparisons; run without --quick)\n";
	}
	else
	{
		std::cout << "F7a alpha > 0 reduces write-backs on zipf (reuse present)\n";
		test_alpha_reduces_writes("zipf", 0.8, true);
		std::cout << "F7b alpha > 0 cannot help on sequential (no reuse)\n";
		test_alpha_reduces_writes("sequential", 0.8, false);
		test_bounded_beats_unbounded();
	}

	test_fault_loop_guard(argv[0]);

	cleanup();

	std::cout << '\n';
	if (g_failures > 0)
	{
		std::cout << "selftest FAILED (" << g_failures << " check(s))\n";
		return 1;
	}
	std::cout << "selftest ok\n";
	return 0;
}
