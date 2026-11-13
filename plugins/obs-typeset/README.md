# Typeset Text

A text source for OBS on Linux, built on Pango, so it can use any font installed on the machine
in any of its weights and styles (Thin to Black, Condensed, Italic), in any script. Two things no
other OBS text source does:

- **It scales like a vector drawing.** Resize it in the scene however you like, by its handles or
  in Edit Transform: it's redrawn at the size it's actually shown, so it's never blurred or
  pixelated, and it's never drawn at more pixels than it covers either.
- **You edit it right in the preview,** the way you'd edit a text box in a slide editor.

## Adding one

Sources, then **+**, then **Typeset Text**. The simple job is the top of its properties: the text,
the font (any weight or style the font has), its size, colour and alignment. New text has a soft
shadow, so it reads over projector visuals and video from the start.

Everything else is in groups that show their settings only when ticked:

| Group | For |
|---|---|
| **Text box** | A fixed width, so the text stays put when the words change. Text that's too long wraps onto more lines, or shrinks to fit (for band names of any length: the font size itself isn't changed). An optional height, with vertical alignment. |
| **Outline** | Colour and width. Drawn outside the letters, so it never thins them. |
| **Shadow** | Colour, offset and blur. With no offset, it's a glow. On by default. |
| **Background** | A plate behind the text: colour, padding, rounded corners. For lower thirds and messages over busy visuals. |
| **Spacing and case** | Line and letter spacing; UPPERCASE, lowercase or Title Case (the text stays as you typed it); even-width numbers, so set times line up. |

**Copy style** and **Paste style** copy everything but the words from one Typeset Text to another;
**Use this style for new text** makes new ones start with it.

Every size here (the font's, the box, outline, shadow, padding) is in the text's own pixels, before
the scene scales it, so the whole design scales together. If a text's font isn't installed on the
computer (a scene collection moved from another machine), its properties say so, and which font
is shown instead.

## Resizing and font size

The font size is the text's size before the scene scales it. Scaling the source in the scene then
scales the text, as it would a picture, except that the text is redrawn for its new size instead of
being stretched. 72 px text scaled 2x looks exactly like 144 px text at 1x, pixel for pixel.

So you can set the size whichever way is handier, in the properties or by dragging, and the result
is the same. OBS's handles always scale (a side handle stretches the letters, sharply); to change
how text wraps, give it a text box and set the box's width.

How it's done: the text is laid out once at its font size, with hinting off and positions not
rounded, so its layout is exactly the same at every scale. Each frame the source looks at how it's
being drawn and, if that's changed, the text is drawn again for that size on a background thread (a
few milliseconds), and the new texture replaces the old. In the output its pixels line up with the
canvas's, so it isn't resampled at all unless it's rotated. Windows that show the source on its own
(the properties dialog's preview, a source projector) get a drawing of their own, at their size,
freed again a few seconds after they close. While you drag a handle you may see the previous drawing
stretched for a frame or two before the sharp one arrives. Nothing is done for a text that hasn't
changed.

A few things in OBS draw a source into a texture of the source's own size first and scale *that*,
and then the text is as sharp as its font size, no sharper: filters on the source; crop, or Scale
Filtering other than Disable, on the scene item; and a scene used inside another scene, when the
outer scene item is scaled. Outlines and shadows reach outside the source's box, and filters and
crop cut them off at it.

## Editing in the preview

Double-click the text in the preview: the word you double-clicked is selected. Or select the text
and press **F2**: all of it is selected, so typing replaces it. The text gets a blue frame and an
"Editing · Esc to finish" tag (in the preview only); then:

| | |
|---|---|
| Type | replaces the selection, or types at the caret |
| Click, drag | place the caret, select (Shift+click extends) |
| Double-click, triple-click | select a word, a whole line |
| Arrows, Home, End | move; Ctrl moves by word (or to the very start or end); Shift selects |
| Backspace, Delete | delete; Ctrl deletes a word |
| Return | a new line |
| Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V | select all, copy, cut, paste (always as plain text) |
| Ctrl+Z, Ctrl+Shift+Z | undo, redo (a word at a time) |
| Escape, Ctrl+Return, or a click outside the text | finish |

While you're editing, every key goes to the text: OBS's shortcuts (Delete removing the source, the
arrows nudging it) and its hotkeys (a scene switched by the 1 key) are back once you finish. The
caret, selection and frame are drawn in the preview only, never into the output, so they don't show
on the stream, the recording or a projector. The text itself changes live, as you type, as any
change in OBS does; in Studio Mode, editing is in the preview's scene.

A Typeset Text with no words shows where it is with a dashed frame and "Double-click to type", in
the preview only.

Editing works with the preview set to **Scale to Window** (OBS's default; right-click the preview,
then Preview Scaling). Zoomed, or at a fixed scale, double-clicking does what it normally does in
OBS, and OBS's log says why.

## How editing hooks into OBS

OBS doesn't give plugins a way to take part in its preview, so the editor works round it:

- It watches the preview's mouse and keyboard events (a Qt event filter on OBS's preview widget)
  and takes only those that are for the text: a double-click on a Typeset Text that's on top
  under the pointer, F2 with one selected, and while editing, keys and clicks on the text.
- It works out where the canvas is in the preview with the same calculation OBS uses, and only
  when OBS's own Preview Scaling menu says Scale to Window. As a second check, the item it finds
  under a double-click must be one OBS has selected (with the first click, or already).
- While editing, OBS's hotkeys don't read the keyboard (`obs_hotkey_enable_background_press`),
  and their setting is put back as OBS has it when editing finishes.
- It draws the caret, selection and placeholders with a draw callback on the preview's display,
  which it finds inside OBS's preview widget (the display's pointer is that widget's first
  member). That's read only in the OBS versions it's known to hold in (27 to 33), and checked
  against the display's size; otherwise editing still works, without the caret.

## Tests

[tests/typeset](../../tests/typeset) runs the installed OBS on a virtual display and checks that
text scaled in a scene comes out identical to text at the matching font size; and, using the mouse
and keyboard on OBS's window, that editing in the preview works, never shows in the output, isn't
interrupted by OBS's shortcuts or hotkeys, and works in Studio Mode; and the text box's sizes.
`capture.py` there takes a fixed set of screenshots for reviewing the design.

## Building

Needs Pango (1.50 or later) and Qt 6's development files besides what `install.sh` already needs:
`sudo apt install libpango1.0-dev qt6-base-dev`.
