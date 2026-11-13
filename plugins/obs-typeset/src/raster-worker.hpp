// One background thread that rasterises text for every source, so neither
// OBS's graphics thread nor its window ever waits for text to be drawn.
// Requests are coalesced: while one is being drawn, newer ones for the same
// source replace each other and only the latest is drawn next.
#pragma once

#include "text-engine.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace typeset {

struct RasterJob {
	Document doc;
	RasterParams params;
	uint64_t seq = 0;
	uint64_t version = 0; // of the document
	uint32_t source_width = 0, source_height = 0; // the source's size for it
};

// Shared by a source, the worker and OBS's graphics thread.
struct RasterSlot {
	std::mutex mutex;
	std::optional<RasterJob> job;  // waiting to be drawn
	std::optional<Raster> result;  // drawn, waiting to be uploaded
	uint64_t result_seq = 0;
	uint64_t discard_through = 0; // drawings asked for up to here aren't wanted any more
	bool queued = false;
};

void worker_start();
void worker_stop();
void worker_submit(const std::shared_ptr<RasterSlot> &slot, RasterJob job);

} // namespace typeset
