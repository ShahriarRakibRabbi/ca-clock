#pragma once

#include <vector>

enum PageState
{
	NOT_RESIDENT = 0,
	RESIDENT = 1
};

struct PageEntry
{
	PageState state;
	bool referenced;
	bool dirty;
	int frame_index;

	// How many times the clock hand has already skipped this page for being
	// dirty. Used only by the bounded-k policy; reset whenever the page is
	// referenced again (it has proved it is still in use) or freshly paged in.
	int dirty_skips;
};

struct PageTable
{
	std::vector<PageEntry> entries;
};

PageTable create_pagetable(int total_pages);
void dump_pagetable(const PageTable &table);