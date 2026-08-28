#include "workload.h"

#include <algorithm> // std::lower_bound
#include <cassert>
#include <cmath>	 // std::pow
#include <random>

Workload create_sequential_workload(int total_pages, unsigned seed)
{
	assert(total_pages > 0);
	Workload wl;
	wl.type = "sequential";
	wl.total_pages = total_pages;
	wl.seed = seed;
	wl.counter = 0;
	wl.cdf.clear();
	wl.rng.seed(seed);
	return wl;
}

Workload create_zipf_workload(int total_pages, double s, unsigned seed)
{
	assert(total_pages > 0);
	assert(s > 0.0);
	Workload wl;
	wl.type = "zipf";
	wl.total_pages = total_pages;
	wl.seed = seed;
	wl.counter = 0;
	wl.cdf.clear();
	wl.rng.seed(seed);

	// Compute Zipf probabilities p_k = 1 / k^s, normalized
	wl.cdf.resize(static_cast<std::vector<double>::size_type>(total_pages));
	double sum = 0.0;
	for (int k = 1; k <= total_pages; ++k)
	{
		sum += 1.0 / std::pow(static_cast<double>(k), s);
	}

	double cumulative = 0.0;
	for (int k = 1; k <= total_pages; ++k)
	{
		double pk = (1.0 / std::pow(static_cast<double>(k), s)) / sum;
		cumulative += pk;
		wl.cdf[static_cast<std::vector<double>::size_type>(k - 1)] = cumulative;
	}

	// Ensure last element is exactly 1.0
	if (!wl.cdf.empty())
	{
		wl.cdf.back() = 1.0;
	}

	return wl;
}

int workload_next(Workload &wl)
{
	assert(wl.total_pages > 0);
	if (wl.type == "sequential")
	{
		const long idx = wl.counter++;
		return static_cast<int>(idx % wl.total_pages);
	}

	// Zipf: one uniform draw, then binary-search the precomputed CDF.
	std::uniform_real_distribution<double> dist(0.0, 1.0);
	const double u = dist(wl.rng);
	wl.counter++;

	const std::vector<double>::iterator it = std::lower_bound(wl.cdf.begin(), wl.cdf.end(), u);
	if (it == wl.cdf.end())
	{
		return wl.total_pages - 1;
	}
	return static_cast<int>(std::distance(wl.cdf.begin(), it));
}

void destroy_workload(Workload &wl)
{
	wl.cdf.clear();
	wl.type.clear();
}
