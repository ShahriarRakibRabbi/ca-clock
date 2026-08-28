#include "stats.h"

#include <fstream>
#include <iomanip>
#include <sstream>

Stats create_stats()
{
	Stats s;
	s.read_accesses = 0;
	s.write_accesses = 0;

	s.faults_total = 0;
	s.faults_pagein = 0;
	s.faults_write_protect = 0;
	s.faults_reference = 0;

	s.evictions = 0;
	s.pagein_reads = 0;
	s.writeback_writes = 0;
	s.dirty_spares = 0;
	s.scan_exhausted = 0;

	s.io_read_us_total = 0.0;
	s.io_write_us_total = 0.0;
	s.ewma_read_us = 0.0;
	s.ewma_write_us = 0.0;
	s.wall_clock_us = 0.0;

	s.checksum = 0ULL;
	return s;
}

double stats_r_hat(const Stats &s)
{
	if (s.ewma_read_us <= 0.0)
	{
		return 0.0;
	}
	return s.ewma_write_us / s.ewma_read_us;
}

double stats_estimated_io_us(const Stats &s)
{
	return static_cast<double>(s.pagein_reads) * s.ewma_read_us +
		   static_cast<double>(s.writeback_writes) * s.ewma_write_us;
}

const char *stats_policy_name(double alpha, int max_dirty_skips)
{
	if (max_dirty_skips > 0)
	{
		return "bounded";
	}
	if (alpha > 0.0)
	{
		return "prob";
	}
	return "clock";
}

static std::string csv_header()
{
	return std::string("label,policy,pattern,total_pages,frame_budget,alpha,max_dirty_skips,") +
		   "write_ratio,durable_writes,seed," +
		   "read_accesses,write_accesses," +
		   "faults_total,faults_pagein,faults_write_protect,faults_reference," +
		   "evictions,pagein_reads,writeback_writes,dirty_spares,scan_exhausted," +
		   "io_read_us_total,io_write_us_total,ewma_read_us,ewma_write_us,r_hat," +
		   "est_io_us,wall_clock_us,checksum\n";
}

bool stats_append_csv_row(const std::string &path, const std::string &label, const std::string &pattern,
						  int total_pages, int frame_budget, double alpha, int max_dirty_skips,
						  double write_ratio, bool durable_writes,
						  unsigned seed, const Stats &s, std::string &error_message)
{
	std::ifstream probe(path.c_str());
	const bool exists = probe.good();
	probe.close();

	std::ofstream out(path.c_str(), std::ios::app);
	if (!out.is_open())
	{
		std::ostringstream message;
		message << "could not open CSV path " << path;
		error_message = message.str();
		return false;
	}

	if (!exists)
	{
		out << csv_header();
	}

	out << label << ',' << stats_policy_name(alpha, max_dirty_skips) << ',' << pattern << ','
		<< total_pages << ',' << frame_budget << ','
		<< std::fixed << std::setprecision(3) << alpha << ',' << max_dirty_skips << ','
		<< write_ratio << ',' << (durable_writes ? "true" : "false") << ',' << seed << ','
		<< s.read_accesses << ',' << s.write_accesses << ','
		<< s.faults_total << ',' << s.faults_pagein << ',' << s.faults_write_protect << ',' << s.faults_reference << ','
		<< s.evictions << ',' << s.pagein_reads << ',' << s.writeback_writes << ','
		<< s.dirty_spares << ',' << s.scan_exhausted << ','
		<< std::fixed << std::setprecision(2)
		<< s.io_read_us_total << ',' << s.io_write_us_total << ','
		<< s.ewma_read_us << ',' << s.ewma_write_us << ',' << stats_r_hat(s) << ','
		<< stats_estimated_io_us(s) << ',' << s.wall_clock_us << ','
		<< s.checksum << '\n';

	if (!out.good())
	{
		error_message = "failed while writing CSV row to " + path;
		return false;
	}

	out.close();
	return true;
}
