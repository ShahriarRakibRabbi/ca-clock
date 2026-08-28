#include "pagetable.h"

#include <cassert>
#include <iostream>

PageTable create_pagetable(int total_pages)
{
	assert(total_pages >= 0);
	PageTable table;
	table.entries.resize(static_cast<std::vector<PageEntry>::size_type>(total_pages));
	for (std::vector<PageEntry>::size_type index = 0; index < table.entries.size(); ++index)
	{
		table.entries[index].state = NOT_RESIDENT;
		table.entries[index].referenced = false;
		table.entries[index].dirty = false;
		table.entries[index].frame_index = -1;
		table.entries[index].dirty_skips = 0;
	}
	return table;
}

void dump_pagetable(const PageTable &table)
{
	for (std::vector<PageEntry>::size_type index = 0; index < table.entries.size(); ++index)
	{
		const PageEntry &entry = table.entries[index];
		char symbol = '.';
		if (entry.state == RESIDENT)
		{
			symbol = entry.dirty ? 'D' : 'r';
		}
		std::cout << symbol;
	}
	std::cout << '\n';
}