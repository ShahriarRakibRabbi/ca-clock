#include "store.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <random>
#include <sstream>
#include <vector>

namespace
{
	const double EWMA_WEIGHT = 0.1;

	void set_error(std::string &error_message, const char *what, DWORD error_code)
	{
		std::ostringstream message;
		message << what << " failed with GetLastError()=" << error_code;
		error_message = message.str();
	}

	/*
	QueryPerformanceCounter is the only clock on Windows with sub-microsecond
	resolution that is safe to call here. A single 4 KB unbuffered read can take
	well under 100 us on an SSD, which GetTickCount (15 ms granularity) cannot
	see at all, so anything coarser would report every I/O as taking zero time.
	*/
	double qpc_frequency()
	{
		static double frequency = 0.0;
		if (frequency == 0.0)
		{
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			frequency = static_cast<double>(f.QuadPart);
		}
		return frequency;
	}

	LONGLONG qpc_now()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	double qpc_elapsed_us(LONGLONG start)
	{
		return static_cast<double>(qpc_now() - start) * 1000000.0 / qpc_frequency();
	}

	void update_ewma(double &ewma, double sample)
	{
		if (ewma == 0.0)
		{
			ewma = sample;
		}
		else
		{
			ewma = (1.0 - EWMA_WEIGHT) * ewma + EWMA_WEIGHT * sample;
		}
	}

	std::string::size_type last_slash(const std::string &path)
	{
		const std::string::size_type backslash = path.find_last_of('\\');
		const std::string::size_type slash = path.find_last_of('/');
		if (backslash == std::string::npos)
		{
			return slash;
		}
		if (slash == std::string::npos)
		{
			return backslash;
		}
		return backslash > slash ? backslash : slash;
	}

	bool ensure_parent_directory(const std::string &path, std::string &error_message)
	{
		const std::string::size_type separator = last_slash(path);
		if (separator == std::string::npos)
		{
			return true;
		}

		const std::string directory = path.substr(0, separator);
		if (directory.empty())
		{
			return true;
		}

		if (CreateDirectoryA(directory.c_str(), NULL) == 0)
		{
			const DWORD error_code = GetLastError();
			if (error_code != ERROR_ALREADY_EXISTS)
			{
				set_error(error_message, "CreateDirectoryA", error_code);
				return false;
			}
		}

		return true;
	}

	void fill_known_pattern(unsigned char *buffer, int page_index)
	{
		for (long offset = 0; offset < PAGE_BYTES; ++offset)
		{
			buffer[offset] = static_cast<unsigned char>((page_index + offset) & 0xFF);
		}
	}

	bool seek_to_page(HANDLE handle, int page_index, std::string &error_message)
	{
		LARGE_INTEGER position;
		position.QuadPart = static_cast<LONGLONG>(page_index) * PAGE_BYTES;
		if (SetFilePointerEx(handle, position, NULL, FILE_BEGIN) == 0)
		{
			set_error(error_message, "SetFilePointerEx", GetLastError());
			return false;
		}
		return true;
	}

	bool write_exact(HANDLE handle, const unsigned char *buffer, std::string &error_message)
	{
		DWORD written = 0;
		if (WriteFile(handle, buffer, PAGE_BYTES, &written, NULL) == 0 || written != PAGE_BYTES)
		{
			set_error(error_message, "WriteFile", GetLastError());
			return false;
		}
		return true;
	}

	bool read_exact(HANDLE handle, unsigned char *buffer, std::string &error_message)
	{
		DWORD read = 0;
		if (ReadFile(handle, buffer, PAGE_BYTES, &read, NULL) == 0 || read != PAGE_BYTES)
		{
			set_error(error_message, "ReadFile", GetLastError());
			return false;
		}
		return true;
	}

	/*
	Reset the backing file to the known pattern.

	This uses a plain BUFFERED handle on purpose. Resetting is setup, not
	measurement: writing 32 MB through the Windows cache takes a fraction of a
	second, whereas the same 8192 writes unbuffered take tens of seconds and would
	dominate every run. None of this time is timed or reported — the experiment
	reopens the file with FILE_FLAG_NO_BUFFERING immediately afterwards, so every
	number the project publishes still comes from real device I/O.

	Resetting matters for reproducibility: without it, run N reads whatever run
	N-1 left behind, and two runs of the same configuration are not comparable.
	*/
	/*
	On Windows a file that was just closed can still be briefly unopenable:
	antivirus and the search indexer open newly written files to scan them, and
	CreateFile then fails with ERROR_SHARING_VIOLATION. This is not a bug in our
	code and it is not permanent, so retry briefly before giving up. Running the
	sweep back-to-back hits this reliably without the retry.
	*/
	HANDLE create_file_with_retry(const std::string &path, DWORD access, DWORD share_mode,
								  DWORD creation, DWORD flags, const char *what,
								  std::string &error_message)
	{
		const int max_attempts = 20;
		for (int attempt = 0; attempt < max_attempts; ++attempt)
		{
			HANDLE handle = CreateFileA(path.c_str(), access, share_mode, NULL, creation, flags, NULL);
			if (handle != INVALID_HANDLE_VALUE)
			{
				return handle;
			}

			const DWORD error_code = GetLastError();
			if (error_code != ERROR_SHARING_VIOLATION && error_code != ERROR_ACCESS_DENIED)
			{
				set_error(error_message, what, error_code);
				return INVALID_HANDLE_VALUE;
			}
			Sleep(50);
		}

		set_error(error_message, what, ERROR_SHARING_VIOLATION);
		error_message += " after retrying for 1 s (antivirus or another process is holding the file)";
		return INVALID_HANDLE_VALUE;
	}

	bool reset_backing_file(const std::string &path, int total_pages, std::string &error_message)
	{
		HANDLE handle = create_file_with_retry(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS,
											   FILE_ATTRIBUTE_NORMAL, "CreateFileA (reset)", error_message);
		if (handle == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		// Buffered writes have no alignment requirement, so an ordinary buffer is fine.
		std::vector<unsigned char> buffer(static_cast<std::vector<unsigned char>::size_type>(PAGE_BYTES));
		bool ok = true;
		for (int page = 0; page < total_pages && ok; ++page)
		{
			fill_known_pattern(&buffer[0], page);
			DWORD written = 0;
			if (WriteFile(handle, &buffer[0], PAGE_BYTES, &written, NULL) == 0 || written != PAGE_BYTES)
			{
				set_error(error_message, "WriteFile (reset)", GetLastError());
				ok = false;
			}
		}

		if (ok && FlushFileBuffers(handle) == 0)
		{
			set_error(error_message, "FlushFileBuffers (reset)", GetLastError());
			ok = false;
		}

		CloseHandle(handle);
		return ok;
	}

	bool open_or_create_backing_file(Store &store, const std::string &path, bool reset_contents, std::string &error_message)
	{
		const LONGLONG expected_size = static_cast<LONGLONG>(store.total_pages) * PAGE_BYTES;

		bool needs_reset = reset_contents;
		if (!needs_reset)
		{
			// Only skip the reset if an existing file is already the right size.
			WIN32_FILE_ATTRIBUTE_DATA attributes;
			if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attributes) == 0)
			{
				needs_reset = true;
			}
			else
			{
				LARGE_INTEGER current_size;
				current_size.HighPart = static_cast<LONG>(attributes.nFileSizeHigh);
				current_size.LowPart = attributes.nFileSizeLow;
				needs_reset = (current_size.QuadPart != expected_size);
			}
		}

		if (needs_reset && !reset_backing_file(path, store.total_pages, error_message))
		{
			return false;
		}

		store.handle = create_file_with_retry(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
											  OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
											  "CreateFileA", error_message);
		if (store.handle == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		LARGE_INTEGER actual_size;
		if (GetFileSizeEx(store.handle, &actual_size) == 0)
		{
			set_error(error_message, "GetFileSizeEx", GetLastError());
			return false;
		}
		if (actual_size.QuadPart != expected_size)
		{
			std::ostringstream message;
			message << "backing file " << path << " is " << actual_size.QuadPart
					<< " bytes, expected " << expected_size;
			error_message = message.str();
			return false;
		}

		return true;
	}

	bool prepare_store_buffer(Store &store, std::string &error_message)
	{
		store.io_buffer = static_cast<unsigned char *>(VirtualAlloc(NULL, PAGE_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (store.io_buffer == NULL)
		{
			set_error(error_message, "VirtualAlloc", GetLastError());
			return false;
		}
		return true;
	}

} // namespace

/*
The file backing this project must bypass the Windows cache so the measurements
reflect the real device. FILE_FLAG_NO_BUFFERING forces us to supply an aligned
buffer and to transfer exactly one page at a time. This is why the store owns
its own VirtualAlloc buffer instead of reusing caller memory.
The file is prefilled with a known pattern so never-written pages are still
predictable, which makes the later demand-paging tests meaningful.
*/
Store open_store(const std::string &path, int total_pages, bool durable_writes, bool reset_contents, std::string &error_message)
{
	assert(total_pages > 0);
	Store store;
	store.handle = INVALID_HANDLE_VALUE;
	store.io_buffer = NULL;
	store.total_pages = total_pages;
	store.durable_writes = durable_writes;
	store_reset_measurements(store);

	if (!ensure_parent_directory(path, error_message))
	{
		return store;
	}

	if (!prepare_store_buffer(store, error_message))
	{
		return store;
	}

	if (!open_or_create_backing_file(store, path, reset_contents, error_message))
	{
		close_store(store);
		return store;
	}

	return store;
}

void close_store(Store &store)
{
	if (store.handle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(store.handle);
		store.handle = INVALID_HANDLE_VALUE;
	}

	if (store.io_buffer != NULL)
	{
		VirtualFree(store.io_buffer, 0, MEM_RELEASE);
		store.io_buffer = NULL;
	}
}

bool read_page(Store &store, int page_index, unsigned char *destination, std::string &error_message)
{
	assert(store.handle != INVALID_HANDLE_VALUE);
	assert(store.io_buffer != NULL);
	assert(page_index >= 0);
	assert(page_index < store.total_pages);

	const LONGLONG started = qpc_now();
	if (!seek_to_page(store.handle, page_index, error_message))
	{
		return false;
	}

	if (!read_exact(store.handle, store.io_buffer, error_message))
	{
		return false;
	}
	const double elapsed_us = qpc_elapsed_us(started);

	std::memcpy(destination, store.io_buffer, PAGE_BYTES);

	++store.read_count;
	store.read_us_total += elapsed_us;
	update_ewma(store.ewma_read_us, elapsed_us);
	return true;
}

bool write_page(Store &store, int page_index, const unsigned char *source, std::string &error_message)
{
	assert(store.handle != INVALID_HANDLE_VALUE);
	assert(store.io_buffer != NULL);
	assert(page_index >= 0);
	assert(page_index < store.total_pages);

	std::memcpy(store.io_buffer, source, PAGE_BYTES);

	// The memcpy above is deliberately outside the timed region: it is our own
	// bookkeeping, not device cost. Everything below is the device.
	const LONGLONG started = qpc_now();
	if (!seek_to_page(store.handle, page_index, error_message))
	{
		return false;
	}

	if (!write_exact(store.handle, store.io_buffer, error_message))
	{
		return false;
	}

	if (store.durable_writes)
	{
		if (FlushFileBuffers(store.handle) == 0)
		{
			set_error(error_message, "FlushFileBuffers", GetLastError());
			return false;
		}
	}
	const double elapsed_us = qpc_elapsed_us(started);

	++store.write_count;
	store.write_us_total += elapsed_us;
	update_ewma(store.ewma_write_us, elapsed_us);
	return true;
}

void store_reset_measurements(Store &store)
{
	store.read_count = 0;
	store.write_count = 0;
	store.read_us_total = 0.0;
	store.write_us_total = 0.0;
	store.ewma_read_us = 0.0;
	store.ewma_write_us = 0.0;
}

namespace
{
	double median_of(std::vector<double> &samples)
	{
		if (samples.empty())
		{
			return 0.0;
		}
		std::sort(samples.begin(), samples.end());
		const std::vector<double>::size_type middle = samples.size() / 2;
		if (samples.size() % 2 == 1)
		{
			return samples[middle];
		}
		return 0.5 * (samples[middle - 1] + samples[middle]);
	}

	double percentile_90_of(const std::vector<double> &sorted_samples)
	{
		if (sorted_samples.empty())
		{
			return 0.0;
		}
		std::vector<double>::size_type index =
			static_cast<std::vector<double>::size_type>(0.9 * (sorted_samples.size() - 1));
		return sorted_samples[index];
	}
} // namespace

bool store_calibrate(const std::string &path, int total_pages, bool durable_writes,
					 int samples, unsigned seed, DeviceCalibration &calibration,
					 std::string &error_message)
{
	calibration.read_us_median = 0.0;
	calibration.write_us_median = 0.0;
	calibration.read_us_p90 = 0.0;
	calibration.write_us_p90 = 0.0;
	calibration.samples = 0;

	if (samples <= 0)
	{
		error_message = "calibration needs at least one sample";
		return false;
	}

	Store store = open_store(path, total_pages, durable_writes, true, error_message);
	if (store.handle == INVALID_HANDLE_VALUE || store.io_buffer == NULL)
	{
		return false;
	}

	// Random pages, not sequential, so the numbers describe the random 4 KB access
	// the pager actually performs rather than the device's sequential best case.
	std::mt19937 rng(seed);
	std::uniform_int_distribution<int> page_picker(0, total_pages - 1);

	std::vector<double> read_samples;
	std::vector<double> write_samples;
	std::vector<unsigned char> scratch(static_cast<std::vector<unsigned char>::size_type>(PAGE_BYTES));
	bool ok = true;

	for (int i = 0; i < samples && ok; ++i)
	{
		store_reset_measurements(store);
		if (!read_page(store, page_picker(rng), &scratch[0], error_message))
		{
			ok = false;
			break;
		}
		read_samples.push_back(store.read_us_total);

		store_reset_measurements(store);
		if (!write_page(store, page_picker(rng), &scratch[0], error_message))
		{
			ok = false;
			break;
		}
		write_samples.push_back(store.write_us_total);
	}

	close_store(store);
	if (!ok)
	{
		return false;
	}

	calibration.read_us_median = median_of(read_samples);   // sorts in place
	calibration.write_us_median = median_of(write_samples); // sorts in place
	calibration.read_us_p90 = percentile_90_of(read_samples);
	calibration.write_us_p90 = percentile_90_of(write_samples);
	calibration.samples = samples;
	return true;
}