# Typeset Text

A text source for OBS on Linux, built on Pango, so it can use any font installed on the machine
in any of its weights and styles (Thin to Black, Condensed, Italic). This first version does the
two things no other OBS text source does:

- **It scales like a vector drawing.** Resize it in the scene however you like, by its handles or
  in Edit Transform: it's redrawn at the size it's actually shown on the canvas, so it's never
  blurred or pixelated, and it's never drawn at more pixels than it covers either.
- **You edit it right in the preview,** the way you'd edit a text box in a slide editor.

## Adding one

Sources, then **+**, then **Typeset Text**. In its properties: the text, the font (any weight or
style the font has), colour, alignment and line spacing.

## Resizing and font size

The font size is the text's size before the scene scales it. Scaling the source in the scene then
scales the text, as it would a picture, except that the text is redrawn for its new size instead of
being stretched. 72 px text scaled 2x looks exactly like 144 px text at 1x, pixel for pixel.

So you can set the size whichever way is handier, in the font or by dragging, and the result is the
same. Stretching it one way only (dragging a side handle) stretches the letters, again sharply.

How it's done: the text is laid out once at its font size, with hinting off and positions not
rounded, so its layout is exactly the same at every scale. Each frame the source looks at how
it's being drawn on the canvas and, if that's changed, the text is drawn again for that size on a
background thread (a few milliseconds), and the new texture replaces the old. Its pixels line up
with the canvas's, so it isn't resampled at all unless it's rotated. While you drag a handle you
may see the previous drawing stretched for a frame or two before the sharp one arrives.

A few things in OBS draw a source into a texture of the source's own size first and scale *that*,
and then the text is as sharp as its font size, no sharper:

- filters on the source,
- crop, or Scale Filtering other than Disable, on the scene item,
- a scene used inside another scene: scaling the outer scene item.

## Editing in the preview

Double-click the text in the preview. The word you double-clicked is selected and the text gets a
blue frame; then:

| | |
|---|---|
| Type | replaces the selection, or types at the caret |
| Click, drag | place the caret, select (Shift+click extends) |
| Double-click, triple-click | select a word, a whole line |
| Arrows, Home, End | move; Ctrl moves by word (or to the very start or end); Shift selects |
| Backspace, Delete | delete; Ctrl deletes a word |
| Return | a new line |
| Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V | select all, copy, cut, paste |
| Ctrl+Z, Ctrl+Shift+Z | undo, redo |
| Escape, or click outside the text | finish |

While you're editing, every key goes to the text: OBS's shortcuts (Delete removing the source, the
arrows nudging it) are back once you finish. The caret, selection and frame are drawn in the
preview only, never into the output, so they don't show on the stream, the recording or a
projector. The text itself does change live, as you type: in Studio Mode it changes in Program too
if the same source is showing there.

Editing works with the preview set to **Scale to Window** (OBS's default; right-click the preview,
then Preview Scaling). Zoomed, or at a fixed scale, double-clicking does what it normally does in
OBS, and OBS's log says why.

## How editing hooks into OBS

OBS doesn't give plugins a way to take part in its preview, so the editor works round it:

- It watches the preview's mouse and keyboard events (a Qt event filter on OBS's preview widget)
  and takes only those that are for the text: a double-click on a Typeset Text that's on top
  under the pointer, and while editing, keys and clicks on the text.
- It works out where the canvas is in the preview with the same calculation OBS uses, and checks
  that against OBS's own: the item it finds under a double-click must be the one OBS has just
  selected with the first click, or it does nothing.
- It draws the caret and selection with a draw callback on the preview's display, which it finds
  inside OBS's preview widget (the display's pointer is that widget's first member). That's
  checked against the display's size before it's used; if it ever doesn't match, editing still
  works, without the caret.

## Tests

[tests/typeset](../../tests/typeset) runs the installed OBS on a virtual display and checks both:
that text scaled in a scene comes out identical to text at the matching font size, and, using the
mouse and keyboard on OBS's window, that editing in the preview works and never shows in the output.

## Building

Needs Pango and Qt 6's development files besides what `install.sh` already needs:
`sudo apt install libpango1.0-dev qt6-base-dev`.
