#include "config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace
{

	std::string trim(const std::string &text)
	{
		std::string::size_type start = 0;
		while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
		{
			++start;
		}

		std::string::size_type finish = text.size();
		while (finish > start && std::isspace(static_cast<unsigned char>(text[finish - 1])))
		{
			--finish;
		}

		return text.substr(start, finish - start);
	}

	bool parse_bool(const std::string &text, bool &value)
	{
		if (text == "true" || text == "1")
		{
			value = true;
			return true;
		}

		if (text == "false" || text == "0")
		{
			value = false;
			return true;
		}

		return false;
	}

	bool parse_long_value(const std::string &text, long &value)
	{
		char *end = 0;
		const long parsed = std::strtol(text.c_str(), &end, 10);
		if (end == text.c_str() || *end != '\0')
		{
			return false;
		}

		value = parsed;
		return true;
	}

	bool parse_int_value(const std::string &text, int &value)
	{
		long parsed = 0;
		if (!parse_long_value(text, parsed))
		{
			return false;
		}

		value = static_cast<int>(parsed);
		return true;
	}

	bool parse_unsigned_value(const std::string &text, unsigned &value)
	{
		char *end = 0;
		const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
		if (end == text.c_str() || *end != '\0')
		{
			return false;
		}

		value = static_cast<unsigned>(parsed);
		return true;
	}

	bool parse_double_value(const std::string &text, double &value)
	{
		char *end = 0;
		const double parsed = std::strtod(text.c_str(), &end);
		if (end == text.c_str() || *end != '\0')
		{
			return false;
		}

		value = parsed;
		return true;
	}

	bool assign_config_value(Config &config, const std::string &key, const std::string &value)
	{
		if (key == "total_pages")
			return parse_int_value(value, config.total_pages);
		if (key == "frame_budget")
			return parse_int_value(value, config.frame_budget);
		if (key == "access_count")
			return parse_long_value(value, config.access_count);
		if (key == "alpha")
			return parse_double_value(value, config.alpha);
		if (key == "max_dirty_skips")
			return parse_int_value(value, config.max_dirty_skips);
		if (key == "write_ratio")
			return parse_double_value(value, config.write_ratio);
		if (key == "zipf_s")
			return parse_double_value(value, config.zipf_s);
		if (key == "pattern")
		{
			config.pattern = value;
			return true;
		}
		if (key == "durable_writes")
			return parse_bool(value, config.durable_writes);
		if (key == "dump_every")
			return parse_int_value(value, config.dump_every);
		if (key == "delay_ms")
			return parse_int_value(value, config.delay_ms);
		if (key == "seed")
			return parse_unsigned_value(value, config.seed);
		if (key == "backing_path")
		{
			config.backing_path = value;
			return true;
		}
		if (key == "csv_path")
		{
			config.csv_path = value;
			return true;
		}
		if (key == "label")
		{
			config.label = value;
			return true;
		}
		return false;
	}

} // namespace

Config default_config()
{
	Config config;
	config.total_pages = 0;
	config.frame_budget = 0;
	config.access_count = 0;
	config.alpha = 0.0;
	config.max_dirty_skips = 0;
	config.write_ratio = 0.0;
	config.zipf_s = 0.99;
	config.pattern = "zipf";
	config.durable_writes = false;
	config.dump_every = 0;
	config.delay_ms = 0;
	config.seed = 0;
	config.backing_path.clear();
	config.csv_path.clear();
	config.label.clear();
	return config;
}

bool load_config_file(const std::string &path, Config &config, std::string &error_message)
{
	config = default_config();

	std::ifstream input(path.c_str());
	if (!input.is_open())
	{
		error_message = "failed to open config file: " + path;
		return false;
	}

	std::string line;
	int line_number = 0;
	while (std::getline(input, line))
	{
		const std::string::size_type comment_position = line.find('#');
		if (comment_position != std::string::npos)
		{
			line = line.substr(0, comment_position);
		}

		++line_number;
		std::string stripped = trim(line);
		if (stripped.empty() || stripped[0] == '#')
		{
			continue;
		}

		const std::string::size_type equals_position = stripped.find('=');
		if (equals_position == std::string::npos)
		{
			std::ostringstream message;
			message << path << ':' << line_number << ": missing '='";
			error_message = message.str();
			return false;
		}

		const std::string key = trim(stripped.substr(0, equals_position));
		const std::string value = trim(stripped.substr(equals_position + 1));
		if (!assign_config_value(config, key, value))
		{
			std::ostringstream message;
			message << path << ':' << line_number << ": invalid value for '" << key << "'";
			error_message = message.str();
			return false;
		}
	}

	return validate_config(config, error_message);
}

/*
Checked here rather than left to assert(), because asserts vanish under NDEBUG
and a frame_budget larger than total_pages would then run straight into
out-of-bounds indexing in the frame ring instead of a clear message.
*/
bool validate_config(const Config &config, std::string &error_message)
{
	std::ostringstream message;
	if (config.total_pages <= 0)
	{
		message << "total_pages must be > 0 (got " << config.total_pages << ")";
	}
	else if (config.frame_budget <= 0 || config.frame_budget > config.total_pages)
	{
		message << "frame_budget must be in 1.." << config.total_pages << " (got " << config.frame_budget << ")";
	}
	else if (config.access_count <= 0)
	{
		message << "access_count must be > 0 (got " << config.access_count << ")";
	}
	else if (config.alpha < 0.0 || config.alpha > 1.0)
	{
		message << "alpha must be in 0.0..1.0 (got " << config.alpha << ")";
	}
	else if (config.max_dirty_skips < 0 || config.max_dirty_skips > 64)
	{
		message << "max_dirty_skips must be in 0..64 (got " << config.max_dirty_skips << ")";
	}
	else if (config.alpha > 0.0 && config.max_dirty_skips > 0)
	{
		message << "set either alpha or max_dirty_skips, not both: they are two different "
				   "policies and combining them makes the result impossible to attribute";
	}
	else if (config.write_ratio < 0.0 || config.write_ratio > 1.0)
	{
		message << "write_ratio must be in 0.0..1.0 (got " << config.write_ratio << ")";
	}
	else if (config.zipf_s <= 0.0)
	{
		message << "zipf_s must be > 0 (got " << config.zipf_s << ")";
	}
	else if (config.pattern != "zipf" && config.pattern != "sequential")
	{
		message << "pattern must be 'zipf' or 'sequential' (got '" << config.pattern << "')";
	}
	else
	{
		return true;
	}

	error_message = message.str();
	return false;
}

void print_config(const Config &config)
{
	std::cout << "total_pages=" << config.total_pages << '\n';
	std::cout << "frame_budget=" << config.frame_budget << '\n';
	std::cout << "access_count=" << config.access_count << '\n';
	std::cout << "alpha=" << config.alpha << '\n';
	std::cout << "max_dirty_skips=" << config.max_dirty_skips << '\n';
	std::cout << "write_ratio=" << config.write_ratio << '\n';
	std::cout << "zipf_s=" << config.zipf_s << '\n';
	std::cout << "pattern=" << config.pattern << '\n';
	std::cout << "durable_writes=" << (config.durable_writes ? "true" : "false") << '\n';
	std::cout << "dump_every=" << config.dump_every << '\n';
	std::cout << "delay_ms=" << config.delay_ms << '\n';
	std::cout << "seed=" << config.seed << '\n';
	std::cout << "backing_path=" << config.backing_path << '\n';
	std::cout << "csv_path=" << config.csv_path << '\n';
	std::cout << "label=" << config.label << '\n';
}