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

Everything else is in groups you tick to turn on; a group's settings show only while it's ticked
(unticked, it says in a line what it does):

| Group | For |
|---|---|
| **Text box** | A fixed width, so the text stays put when the words change. Text that's too long wraps onto more lines, or shrinks to fit (for band names of any length: the font size itself isn't changed). An optional height, with vertical alignment. While editing in the preview, a blue grip on the frame's right edge drags its width. |
| **Outline** | Colour and width. Drawn outside the letters, so it never thins them. |
| **Shadow** | Colour, opacity, offset, blur and spread (how far it reaches past the letters before it softens, so it holds behind thin type). With no offset, it's a glow. On by default. |
| **Background** | A plate behind the text: colour, opacity, padding, rounded corners. For lower thirds and messages over busy visuals. |
| **Spacing and case** | Line and letter spacing; UPPERCASE, lowercase or capitals at the start of each word (the text stays as you typed it); even-width numbers, so set times line up. |

In the **Style** group, **Copy style** and **Paste style** copy everything but the words from one
Typeset Text to another; **Use this style for new text** makes new ones start with it.

Every size here (the font's, the box, outline, shadow, padding) is in the text's own pixels, before
the scene scales it, so the whole design scales together. If a text's font isn't installed on the
computer (a scene collection moved from another machine), its properties say so, and which font
is shown instead.

## Styling words

Part of the text can have a style of its own: a band's name in another typeface or weight, a time in
another colour, the "st" of "21st" raised. While editing in the preview, a format bar sits above the
text, made of OBS's own controls in OBS's theme:

- **Typeface**, **Size** and **Weight**: any font, at any size in the text's own pixels (type it, pick a
  common one, or scroll over it to step it a tenth at a time), in any of its weights (Thin to Black,
  as it has them).
- **Italic**, **Underline**, **Superscript** and **Subscript** (the font's own superscript size and
  height), lit for what the selection has.
- **Colour**, and **Clear formatting** (the eraser), which puts the selection back to the text's own
  style.
- After a line, the whole text's **Alignment**: the one control that's for all of the text, not the
  selection.

Each has a tooltip naming its shortcut:

| | |
|---|---|
| Ctrl+B | bold on and off (a weight: bold is always bolder than the text round it, and in a text whose font is bold already, it makes the selection regular) |
| Ctrl+I, Ctrl+U | italic, underline |
| Ctrl+Shift+=, Ctrl+= | superscript, subscript |
| Ctrl+Shift+> and < (or Ctrl+] and [) | bigger, smaller, a tenth of the size at a time (each part of a mixed selection from its own size) |
| Ctrl+Space | clear formatting |

With nothing selected, a style goes on what you type next.

The properties are the style of the rest of the text: change the font there and the bold words stay
bold, in the new font. A note in the properties says when some words have a style of their own, with
a button to clear them. (Styles set by a script that overlap are tidied up: the earlier one keeps the overlap.) Words typed after a styled word continue its style; text changed in the
properties keeps its styles where it's unchanged.

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
changed. Laying text out (and loading a font the first time it's used) happens on OBS's UI thread,
never the thread that renders the output, so a long text or a new font can't drop frames; a change
from the properties or a script shows a frame or so later for it. The size OBS sees follows the
drawing it shows, so text that isn't anchored at its top left doesn't jump while you type.

Each text has one drawing for the output and one for windows. A text shown twice in the same frame
at different sizes (the same source in two places in a scene) is drawn for the larger and scaled down
for the smaller, which is then a little soft. A text that isn't on screen keeps its drawing on the
graphics card, at the size it was last shown at, so it's ready the moment it's cut to; in Studio
Mode, at the size it has in the preview if that's larger, so a Cut is sharp from its first frame
(the multiview's small panes never make it smaller). A window's drawing
is let go a couple of seconds after the window stops showing the text. If a drawing can't be made
(out of memory), what's shown stays, and it's tried again after 3 seconds, then less often, up to
once a minute (at once if the text changes), with one line in OBS's log.

A few things in OBS draw a source into a texture of the source's own size first and scale *that*,
and then the text is as sharp as its font size, no sharper: filters on the source; crop, Scale
Filtering other than Disable, or a Blending Mode other than Normal (Additive, Screen and so on) on
the scene item; and a scene used inside another scene, when the outer scene item is scaled. Windows
that show a whole scene (the Studio Mode preview, a scene projector, the multiview) show the text as
it's drawn for the canvas, scaled to the window. Outlines and shadows reach outside the source's box, and filters and
crop cut them off at it.

## Editing in the preview

Double-click the text in the preview: the word you double-clicked is selected. Or select the text
and press **F2**: all of it is selected, so typing replaces it. The text gets a frame in OBS's
highlight colour and the format bar above it, in the preview only; then:

| | |
|---|---|
| Type | replaces the selection, or types at the caret |
| Click, drag | place the caret, select (Shift+click extends) |
| Double-click, triple-click | select a word, a whole line |
| Arrows, Home, End | move; Ctrl moves by word (or to the very start or end); Shift selects |
| Backspace, Delete | delete (a letter with its accent, or an emoji, as one; in Indic scripts Backspace takes off just the vowel sign); Ctrl deletes a word |
| Return | a new line |
| Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V | select all, copy, cut, paste (always as plain text, up to 20,000 characters) |
| Ctrl+Z, Ctrl+Shift+Z | undo, redo (a word at a time) |
| Escape, Ctrl+Return, or a click outside the text | finish |

While you're editing, every key goes to the text: OBS's shortcuts (Delete removing the source, the
arrows nudging it) and its hotkeys (a scene switched by the 1 key) are back once you finish. The
caret, selection and frame are drawn in the preview only, never into the output, so they don't show
on the stream, the recording or a projector. The text itself changes live, as you type, as any
change in OBS does; in Studio Mode, editing is in the preview's scene.

The properties dialog keeps what it had when it opened: with it open, words typed in the preview are
undone by its **Cancel** (as any change made elsewhere would be). Each edit in the preview, from
starting to finishing, is one step in OBS's own undo history: **Edit, Undo** (Ctrl+Z, once you've
finished) takes it all back, styles, box width and alignment included, and Redo puts it back. While
you're editing, Ctrl+Z undoes a word at a time instead.

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
  member). That's read only in the OBS versions it's known to hold in, and checked against the
  display's size.
- Each edit is added to OBS's undo history (`obs_frontend_add_undo_redo_action`), naming the source
  by its UUID.

All of this is in `src/obs-preview.cpp`, and is known to hold in OBS 30 to 33 (`known_obs_first` and
`known_obs_last` there); in any other version text isn't edited in the preview (its properties
work as ever) and OBS's log says why.
The editing itself (`src/edit-model.cpp`: the text, its styled runs, caret, selection and undo) has
no OBS or Qt in it, and what's drawn on the preview is in `src/overlay.cpp`.

## Tests

[tests/typeset](../../tests/typeset) runs the installed OBS on a virtual display and checks that
text scaled in a scene comes out identical to text at the matching font size; and, using the mouse
and keyboard on OBS's window, that editing in the preview works, never shows in the output, isn't
interrupted by OBS's shortcuts or hotkeys, and works in Studio Mode; that OBS's Undo and Redo take
a whole edit back and forth; that hiding the text ends its edit at once; and the text box's sizes.
`capture.py` there takes a fixed set of screenshots for reviewing the design. `unit/run.sh` tests the
styled runs, the editing model (typing, undo, bold on and off, size steps, pasting), where the canvas
sits in the preview, and the reading of font style names, on their own, without OBS.

## Building

Needs OBS 30 or later, Pango (1.50 or later) and Qt 6's development files besides what `install.sh`
already needs: `sudo apt install libpango1.0-dev qt6-base-dev`.
