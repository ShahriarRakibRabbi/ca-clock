#pragma once

#include <string>

#include <windows.h>

static const long PAGE_BYTES = 4096;

/*
The store owns the backing file AND the measurement of that file's latency.
Timing lives here rather than in the pager because this is the only place that
actually touches the device: every microsecond recorded below was spent inside a
ReadFile or WriteFile call on the real SSD, not estimated from a model.
r_hat (write cost / read cost) is the number the whole project turns on, so it
must come from these counters and nowhere else.
*/
struct Store
{
	HANDLE handle;
	unsigned char *io_buffer;
	int total_pages;
	bool durable_writes;

	// Real device measurements, accumulated by read_page / write_page.
	long read_count;
	long write_count;
	double read_us_total;
	double write_us_total;
	double ewma_read_us;  // exponentially weighted average, see D4 in PROJECT_SPEC
	double ewma_write_us;
};

// reset_contents = true refills the file with the known pattern before opening,
// so that every run starts from identical bytes. Pass false only when you want to
// inspect what a previous run left behind (the store round-trip test does this).
Store open_store(const std::string &path, int total_pages, bool durable_writes, bool reset_contents, std::string &error_message);
void close_store(Store &store);
bool read_page(Store &store, int page_index, unsigned char *destination, std::string &error_message);
bool write_page(Store &store, int page_index, const unsigned char *source, std::string &error_message);

// Zero the latency counters without closing the file. Used to discard the cost
// of pre-filling the backing file so it does not pollute the experiment's numbers.
void store_reset_measurements(Store &store);

/*
One device calibration: the median cost of a single 4 KB unbuffered read and of a
single 4 KB unbuffered write, in microseconds.

Why this exists. Two runs that issue exactly the same number of reads and writes
can still report wildly different total I/O time, because SSD latency drifts with
background activity and device state — measured here, by a factor of two between
back-to-back runs. Multiplying each run's operation counts by that run's own
measured latency therefore mixes the effect of the policy with the mood of the
disk, and makes runs incomparable.

So the evaluation separates the two. Operation counts come from the runs and are
exactly reproducible. Cost per operation comes from ONE calibration, and the same
two numbers are applied to every run being compared.

Medians rather than means: a single 40 ms outlier from a background process would
drag a mean far more than it distorts the typical operation.
*/
struct DeviceCalibration
{
	double read_us_median;
	double write_us_median;
	double read_us_p90;
	double write_us_p90;
	int samples;
};

bool store_calibrate(const std::string &path, int total_pages, bool durable_writes,
					 int samples, unsigned seed, DeviceCalibration &calibration,
					 std::string &error_message);
