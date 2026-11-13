#include "raster-worker.hpp"

#include <obs.h>
#include <util/platform.h>

#include <cstdlib>
#include <exception>

#include <condition_variable>
#include <deque>
#include <thread>

namespace typeset {

namespace {

struct Worker {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::shared_ptr<RasterSlot>> queue;
	bool stopping = false;
	std::thread thread;
};

Worker *worker = nullptr;

void run(Worker *w)
{
	PangoFontMap *map = pango_cairo_font_map_new();
	PangoContext *context = create_context(map);

	// The last layout made, kept for as long as the text and style are
	// the same: while a source is being resized only the scale changes.
	Document cached_doc;
	PangoLayout *cached = nullptr;

	for (;;) {
		std::shared_ptr<RasterSlot> slot;
		{
			std::unique_lock lock(w->mutex);
			w->wake.wait(lock, [w] { return w->stopping || !w->queue.empty(); });
			if (w->stopping)
				break;
			slot = std::move(w->queue.front());
			w->queue.pop_front();
		}

		std::optional<RasterJob> job;
		{
			std::lock_guard lock(slot->mutex);
			job.swap(slot->job);
			slot->queued = false;
		}
		if (!job)
			continue;

		const uint64_t started = os_gettime_ns();
		Raster raster;
		try {
			if (!cached || !(cached_doc == job->doc)) {
				if (cached)
					g_object_unref(cached);
				cached = nullptr;
				cached = create_layout(context, job->doc);
				cached_doc = job->doc;
			}
			raster = rasterise(cached, job->doc, job->params);
			if (raster.failed)
				blog(LOG_WARNING, "[obs-typeset] couldn't draw text at %.2fx%.2f: out of memory; "
						  "trying again shortly",
				     job->params.sx, job->params.sy);
		} catch (const std::exception &e) {
			// Out of memory, most likely: the text isn't shown, and OBS
			// carries on.
			blog(LOG_WARNING, "[obs-typeset] couldn't draw text: %s", e.what());
			raster = Raster();
			raster.failed = true; // the source keeps what it shows, and asks again later
		}
		// TYPESET_DEBUG=1 in OBS's environment logs every drawing (for the
		// tests, which check that nothing is drawn when nothing changes).
		static const bool debug = getenv("TYPESET_DEBUG") != nullptr;
		if (debug)
			blog(LOG_INFO, "[obs-typeset] drew %dx%d at %.4fx%.4f in %.2f ms", raster.width, raster.height,
			     job->params.sx, job->params.sy, double(os_gettime_ns() - started) / 1e6);

		raster.version = job->version;
		std::lock_guard lock(slot->mutex);
		if (job->seq > slot->result_seq) {
			slot->result = std::move(raster);
			slot->result_seq = job->seq;
		}
	}

	if (cached)
		g_object_unref(cached);
	g_object_unref(context);
	g_object_unref(map);
}

} // namespace

void worker_start()
{
	if (worker)
		return;
	worker = new Worker;
	worker->thread = std::thread(run, worker);
}

void worker_stop()
{
	if (!worker)
		return;
	{
		std::lock_guard lock(worker->mutex);
		worker->stopping = true;
	}
	worker->wake.notify_all();
	worker->thread.join();
	delete worker;
	worker = nullptr;
}

void worker_submit(const std::shared_ptr<RasterSlot> &slot, RasterJob job)
{
	if (!worker)
		return;
	bool enqueue;
	{
		std::lock_guard lock(slot->mutex);
		slot->job = std::move(job);
		enqueue = !slot->queued;
		slot->queued = true;
	}
	if (enqueue) {
		{
			std::lock_guard lock(worker->mutex);
			worker->queue.push_back(slot);
		}
		worker->wake.notify_one();
	}
}

} // namespace typeset
