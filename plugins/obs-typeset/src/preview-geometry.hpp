// Where the canvas is in OBS's preview widget: plain arithmetic, no OBS or Qt,
// so it can be tested on its own.
//
// OBS draws the canvas into its preview widget scaled to fit, centred, inside
// a 10-pixel margin ("Scale to Window", the default). This is OBS's own
// calculation (OBSBasic::ResizePreview and GetScaleAndCenterPos), the same in
// every OBS version the editor runs in (see obs-preview.hpp). Zoomed or at a
// fixed scale it doesn't hold, and the editor doesn't start (OBS's own menu
// says which it is).
#pragma once

namespace typeset {

constexpr int preview_edge = 10;

struct Mapping {
	double x = 0, y = 0, scale = 1; // canvas -> display pixels
	double dpr = 1;                 // display pixels per widget unit
};

// For a preview widget of (cx, cy) display pixels showing a canvas of
// (base_cx, base_cy), with dpr display pixels per widget unit. The int and
// float steps are OBS's own, so the result matches it to the pixel.
inline Mapping fit_canvas(int cx, int cy, int base_cx, int base_cy, double dpr)
{
	Mapping m;
	m.dpr = dpr;
	const int window_cx = cx - preview_edge * 2, window_cy = cy - preview_edge * 2;
	if (base_cx <= 0 || base_cy <= 0 || window_cx <= 0 || window_cy <= 0)
		return m;

	const double window_aspect = double(window_cx) / double(window_cy);
	const double base_aspect = double(base_cx) / double(base_cy);
	int new_cx, new_cy;
	float scale;
	if (window_aspect > base_aspect) {
		scale = float(window_cy) / float(base_cy);
		new_cx = int(double(window_cy) * base_aspect);
		new_cy = window_cy;
	} else {
		scale = float(window_cx) / float(base_cx);
		new_cx = window_cx;
		new_cy = int(float(window_cx) / base_aspect);
	}
	m.x = double(window_cx / 2 - new_cx / 2 + preview_edge);
	m.y = double(window_cy / 2 - new_cy / 2 + preview_edge);
	m.scale = double(scale);
	return m;
}

} // namespace typeset
