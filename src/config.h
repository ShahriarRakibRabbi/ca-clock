#pragma once

#include <string>

struct Config
{
	int total_pages;
	int frame_budget;
	long access_count;
	double alpha;		 // 0.0 = plain CLOCK; > 0 = CA-CLOCK probabilistic sparing
	int max_dirty_skips; // 0 = off; k > 0 = CA-CLOCK bounded second chance (overrides alpha)
	double write_ratio;
	double zipf_s;	   // Zipf skew; higher = more concentrated working set
	std::string pattern;
	bool durable_writes;
	int dump_every;
	int delay_ms;
	unsigned seed;
	std::string backing_path;
	std::string csv_path;
	std::string label;
};

Config default_config();
bool load_config_file(const std::string &path, Config &config, std::string &error_message);
bool validate_config(const Config &config, std::string &error_message);
void print_config(const Config &config);