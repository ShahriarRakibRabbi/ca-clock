#include "config.h"
#include "experiment.h"
#include "stats.h"
#include "store.h" // PAGE_BYTES

#include <windows.h>
#include <psapi.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

namespace
{
	/*
	The working-set line is the project's proof that the frame budget is enforced
	by Windows and not merely tracked in a variable. GetProcessMemoryInfo reports
	physical memory the kernel has actually committed to this process; if our
	MEM_DECOMMIT calls were fake, this number would grow with the whole arena
	instead of staying near frame_budget * 4 KB.
	*/
	void print_working_set(int frame_budget)
	{
		PROCESS_MEMORY_COUNTERS counters;
		if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0)
		{
			std::cout << "[proof]   working set unavailable, GetLastError()=" << GetLastError() << '\n';
			return;
		}

		const double working_set_mb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
		const double budget_mb = static_cast<double>(frame_budget) * PAGE_BYTES / (1024.0 * 1024.0);
		std::cout << std::fixed << std::setprecision(2)
				  << "[proof]   peak working set " << working_set_mb << " MB"
				  << "   (frame budget = " << budget_mb << " MB + program overhead)\n";
	}

	/*
	Calibration mode. Writes results/device.csv, which the analysis reads to
	convert every run's operation counts into a comparable device cost. Run it
	once per sitting, on an otherwise idle machine, before a sweep.
	*/
	int run_calibration(const Config &config)
	{
		DeviceCalibration calibration;
		std::string error_message;
		const int samples = 200;
		const std::string path = config.backing_path.empty() ? std::string("results/backing.dat") : config.backing_path;

		std::cout << "[calibrate] " << samples << " random 4 KB reads and writes on " << path
				  << (config.durable_writes ? " (durable)" : " (buffered write-through)") << "\n";

		if (!store_calibrate(path, config.total_pages, config.durable_writes, samples, config.seed, calibration, error_message))
		{
			std::cerr << "calibration failed: " << error_message << '\n';
			return 1;
		}

		const double r_hat = (calibration.read_us_median > 0.0)
								 ? calibration.write_us_median / calibration.read_us_median
								 : 0.0;

		std::cout << std::fixed << std::setprecision(2)
				  << "[device]    read  median " << calibration.read_us_median << " us   p90 " << calibration.read_us_p90 << " us\n"
				  << "[device]    write median " << calibration.write_us_median << " us   p90 " << calibration.write_us_p90 << " us\n"
				  << "[device]    r_hat = write/read = " << r_hat << '\n';

		std::ofstream out("results/device.csv");
		if (!out.is_open())
		{
			std::cerr << "could not write results/device.csv\n";
			return 1;
		}
		out << "samples,durable_writes,read_us_median,write_us_median,read_us_p90,write_us_p90,r_hat\n";
		out << calibration.samples << ',' << (config.durable_writes ? "true" : "false") << ','
			<< std::fixed << std::setprecision(3)
			<< calibration.read_us_median << ',' << calibration.write_us_median << ','
			<< calibration.read_us_p90 << ',' << calibration.write_us_p90 << ',' << r_hat << '\n';
		out.close();

		std::cout << "[csv]       wrote results/device.csv\n";
		return 0;
	}
} // namespace

int main(int argc, char **argv)
{
	std::string config_path = "config/debug.ini";
	bool calibrate_only = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::string argument = argv[i];
		if (argument == "--calibrate")
		{
			calibrate_only = true;
		}
		else if (argument == "--help" || argument == "-h")
		{
			std::cout << "usage: ca-clock.exe [config.ini] [--calibrate]\n"
					  << "  config.ini   run one experiment and append a CSV row\n"
					  << "  --calibrate  measure this machine's 4 KB read/write latency\n"
					  << "               and write results/device.csv\n";
			return 0;
		}
		else
		{
			config_path = argument;
		}
	}

	Config config = default_config();
	std::string error_message;
	if (!load_config_file(config_path, config, error_message))
	{
		std::cerr << "failed to load config: " << error_message << '\n';
		return 1;
	}

	if (calibrate_only)
	{
		return run_calibration(config);
	}

	const double budget_percent = 100.0 * config.frame_budget / config.total_pages;
	std::cout << std::fixed << std::setprecision(2)
			  << "[config]  " << config.total_pages << " pages, "
			  << config.frame_budget << " frames (" << budget_percent << "%), "
			  << config.access_count << " accesses, "
			  << config.pattern << (config.durable_writes ? ", durable" : ", buffered") << '\n';
	std::cout << "[policy]  " << stats_policy_name(config.alpha, config.max_dirty_skips);
	if (config.max_dirty_skips > 0)
	{
		std::cout << " (k=" << config.max_dirty_skips << ")";
	}
	else if (config.alpha > 0.0)
	{
		std::cout << " (alpha=" << config.alpha << ")";
	}
	std::cout << ", writes=" << (config.write_ratio * 100.0) << "%\n";

	Stats stats;
	if (!run_experiment(config, stats, error_message))
	{
		std::cerr << "experiment failed: " << error_message << '\n';
		return 1;
	}

	std::cout << std::fixed << std::setprecision(2)
			  << "[device]  read " << stats.ewma_read_us << " us   write " << stats.ewma_write_us
			  << " us   r_hat = " << stats_r_hat(stats) << '\n';
	std::cout << "[result]  faults " << stats.faults_total
			  << "  page-ins " << stats.faults_pagein
			  << "  write-protect " << stats.faults_write_protect
			  << "  reference " << stats.faults_reference
			  << "  evictions " << stats.evictions << '\n';
	std::cout << "[policy]  dirty pages spared " << stats.dirty_spares
			  << "  scan exhausted " << stats.scan_exhausted
			  << " of " << stats.evictions << " evictions\n";
	std::cout << "[io]      page-in reads " << stats.pagein_reads
			  << "  write-backs " << stats.writeback_writes
			  << "  estimated device time " << (stats_estimated_io_us(stats) / 1000.0) << " ms"
			  << "  wall clock " << (stats.wall_clock_us / 1000.0) << " ms\n";
	print_working_set(config.frame_budget);

	const std::string csv_path = config.csv_path.empty() ? std::string("results/runs.csv") : config.csv_path;
	if (!stats_append_csv_row(csv_path,
							  config.label.empty() ? std::string("run") : config.label,
							  config.pattern, config.total_pages, config.frame_budget,
							  config.alpha, config.max_dirty_skips, config.write_ratio,
							  config.durable_writes, config.seed, stats, error_message))
	{
		std::cerr << "failed to write CSV: " << error_message << '\n';
		return 1;
	}

	std::cout << "[csv]     appended to " << csv_path
			  << "   (checksum " << stats.checksum << ")\n";
	return 0;
}
