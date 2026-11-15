// The font browser: a pop-up for finding the right typeface among every
// installed one (the whole Google Fonts library, say), opened from the
// format bar's typeface field and from the text's properties.
//
// Each family is a row: its name and what it has (category, weights, italic)
// in OBS's own font, and the person's own words set in it at their weight.
// Search (names, and categories, classes, feel and designers as words),
// category chips, "covers my text", "has my weight", "has italic"; most used
// first or A-Z. With nothing typed: Recent, Favourites, the typefaces in this
// scene collection, then all. Enter or a click uses the highlighted one;
// Escape, or a click outside, leaves the text as it was. Nothing reaches the
// text until then.
//
// The samples are drawn on a thread of its own and kept, so scrolling through
// ~2,000 families never waits on them; names show at once.
#pragma once

#include <obs.h>

#include <QRect>
#include <QWidget>

#include <functional>
#include <string>
#include <vector>

namespace typeset {

struct FontRequest {
	std::string family;               // the text's typeface now ("" mixed)
	int weight = 400;                 // and weight (the samples are set at it)
	bool italic = false;
	std::string sample;               // the person's words, set in each row
	std::vector<std::string> in_use;  // typefaces used in this scene collection
};

// Opens the browser (one at a time), as a pop-up of parent, by anchor (a
// rectangle on the screen it's opened from: placed just below it) and clear
// of avoid (the text: not covered if there's room elsewhere). search: typed
// to open it. chosen is called with the family used; closed, whatever happens.
void open_font_browser(QWidget *parent, const QRect &anchor, const QRect &avoid, const FontRequest &request,
		       const std::string &search, std::function<void(const std::string &)> chosen,
		       std::function<void()> closed);

// The typefaces Typeset Texts use in this scene collection, each once.
std::vector<std::string> fonts_in_use();

// From a text's properties ("Browse typefaces…"): the family chosen is the
// whole text's. Callable from any thread: it opens on the UI thread.
void browse_fonts_for(obs_source_t *source);

} // namespace typeset
