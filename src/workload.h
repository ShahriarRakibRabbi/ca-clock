#pragma once

#include <vector>
#include <string>
#include <random>

/*
Workload generators: sequential and Zipf.

The random engine is stored in the Workload and advanced once per draw. An
earlier version re-seeded a fresh engine on every call and replayed it from the
start, which made generating N page indices cost O(N^2) — 200,000 accesses spent
hours inside the generator before touching a single page. Keeping one engine
alive makes each draw O(log total_pages), all of it in the binary search.
*/
struct Workload
{
	std::string type; // "sequential" or "zipf"
	int total_pages;
	unsigned seed;
	long counter;			 // for sequential
	std::vector<double> cdf; // cumulative distribution over pages (size == total_pages)
	std::mt19937 rng;
};

// Create a sequential workload (round-robin)
Workload create_sequential_workload(int total_pages, unsigned seed);

// Create a zipf workload with parameter s (typical 0.9..1.0). Precomputes the CDF.
Workload create_zipf_workload(int total_pages, double s, unsigned seed);

// Return the next page index in the trace.
int workload_next(Workload &wl);

// Free any resources (keeps API symmetric)
void destroy_workload(Workload &wl);
