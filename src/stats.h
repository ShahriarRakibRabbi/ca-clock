#pragma once

#include <string>

/*
One run's measurements. Every field below is either counted by the fault handler
or timed with QueryPerformanceCounter around a real ReadFile/WriteFile — nothing
here is modelled or assumed.

The column names are deliberately unambiguous about *what* is being counted,
because the previous version had a single "pages_written" that sometimes meant
"application write accesses" and sometimes meant "4 KB pages sent to the SSD".
Those two numbers differ by more than an order of magnitude and confusing them
makes the whole write-reduction result meaningless.

  read_accesses / write_accesses   what the workload asked for (application level)
  faults_*                         real EXCEPTION_ACCESS_VIOLATION events
  pagein_reads                     4 KB reads issued to the backing file
  writeback_writes                 4 KB writes issued to the backing file  <-- the metric
  est_io_us                        pagein_reads * ewma_read + writeback_writes * ewma_write
*/
struct Stats
{
	long read_accesses;
	long write_accesses;

	long faults_total;
	long faults_pagein;
	long faults_write_protect;
	long faults_reference;

	long evictions;
	long pagein_reads;
	long writeback_writes;

	// Policy behaviour. dirty_spares counts second chances granted to dirty pages.
	// scan_exhausted counts sweeps that found no eligible victim at all and had to
	// evict whatever sat under the hand — if this is not near zero, alpha is high
	// enough that CA-CLOCK has stopped being CLOCK and any result must say so.
	long dirty_spares;
	long scan_exhausted;

	double io_read_us_total;
	double io_write_us_total;
	double ewma_read_us;
	double ewma_write_us;
	double wall_clock_us;

	unsigned long long checksum; // stops the optimiser deleting the workload
};

Stats create_stats();

// r_hat = ewma_write_us / ewma_read_us. Returns 0 when no read has been timed yet.
double stats_r_hat(const Stats &s);

// Total device time attributable to paging, using this machine's own measured
// per-operation latencies. This is the single number that lets a page-in and a
// write-back be compared on the same scale.
double stats_estimated_io_us(const Stats &s);

// "clock", "prob", or "bounded" - which of the three policies a run used. Written
// to the CSV so the analysis can separate the arms without re-deriving them from
// alpha and k, and so a reader can never mistake one arm for another.
const char *stats_policy_name(double alpha, int max_dirty_skips);

// Append one CSV row; writes the header if the file does not exist yet.
bool stats_append_csv_row(const std::string &path, const std::string &label, const std::string &pattern,
						  int total_pages, int frame_budget, double alpha, int max_dirty_skips,
						  double write_ratio, bool durable_writes,
						  unsigned seed, const Stats &s, std::string &error_message);
