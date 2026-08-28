#pragma once

#include "config.h"
#include "stats.h"

#include <string>

bool run_experiment(const Config &config, Stats &stats, std::string &error_message);
