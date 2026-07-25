# Plan: Kitty Graphics Protocol Support for this Windows Terminal Fork

## What we're trying to do

Add Kitty Graphics Protocol support to this fork of Windows Terminal: image
transmission, direct (cursor-relative) placement, **and** Unicode Placeholder
("virtual") placement. Unicode Placeholder support is the actual goal, not a stretch
extra — it's what lets an inline image behave like ordinary text (survives scrolling,
window resize, and reflow) instead of being glued to a fixed screen position. Direct
placement alone is a useful, smaller first milestone but does not satisfy the goal on
its own.

Full context, architecture notes, and the required non-functional properties
(accessibility, security, reliability, compatibility, performance) are written up in
`doc/specs/Kitty Graphics Protocol Support.md` in this same repo — read that first for
the complete picture. This file is the top-level entry point and working plan.

## Why

Windows Terminal implements Sixel but not the Kitty Graphics Protocol. Sixel images are
drawn at the cursor and simply scroll with the text — there's no way to reference
already-transmitted image data cheaply on a later redraw, no placement-by-reference, and
no way to free/evict a previously-shown image's memory. The Kitty protocol's Unicode
Placeholder mechanism solves this: a placement is expressed as ordinary text-buffer
cells (a reserved placeholder code point plus combining marks encoding a logical grid
position, with the image/placement id encoded in the cell's foreground color), so it
rides through the terminal's existing scroll/reflow/copy pipeline for free, and image
data can be transmitted once and referenced repeatedly.

This capability was requested upstream in `microsoft/terminal` and closed **won't-fix**
(reluctance to add a third raster graphics protocol). This fork exists specifically to
ship it anyway, for anyone who wants it, independent of the upstream product's roadmap.

## Reference protocol

The Kitty Graphics Protocol is a public, documented, open specification — not
proprietary to any product:

- Full spec: https://sw.kovidgoyal.net/kitty/graphics-protocol/
- Key mechanisms to implement:
  - **Transmission**: `ESC _G <key>=<value>,... ; <base64 payload> ESC \` (an APC —
    Application Programming Command — escape sequence), chunked for large payloads via
    the `m=1`/`m=0` continuation flags.
  - **Direct placement**: draw a transmitted image at the current cursor cell.
  - **Unicode Placeholder (virtual) placement**: reserve a rectangular grid of cells,
    write the placeholder code point `U+10EEEE` into each with row/column combining
    diacritics identifying that cell's position within the grid, and set the cell's
    24-bit foreground color to encode the image/placement id. The renderer must, at
    paint time, recognize this pattern in place of normal glyph rendering and blit the
    corresponding sub-rectangle of the referenced image instead.
  - **Deletion/free**: `a=d,d=i,...` deletes a placement while keeping image data
    resident (for reuse); `a=d,d=I,...` frees the image data entirely. A well-behaved
    server implementation should support both so clients that track and evict their own
    shown images round-trip correctly.
- Reference implementations worth cross-checking wire-format behavior against: the
  kitty terminal itself (Linux/macOS; the protocol's origin and most complete
  implementation) and WezTerm (cross-platform, including a native Windows build;
  implements transmission and direct placement, but not Unicode Placeholder mode as of
  this writing — useful as a partial-support comparison point, not a full reference).

## Capability signaling contract

This fork should set the environment variable `WT_KITTY_SUPPORTED=true` for sessions it
hosts, **in addition to** (not instead of) the stock `WT_SESSION` variable Windows
Terminal already sets. Do not set `TERM_PROGRAM`, `WEZTERM_PANE`, or `KITTY_WINDOW_ID`
to values implying this is a different terminal — other programs in the same session
read those variables for unrelated purposes, and impersonating another terminal's
identity risks confusing them. `WT_KITTY_SUPPORTED` should only be set once the
corresponding capability tier is genuinely functional in a given build, not
speculatively ahead of the implementation landing.

Clients wishing to detect this fork's Kitty Graphics Protocol support should check for
`WT_SESSION` set **and** `WT_KITTY_SUPPORTED === "true"`.

## Architecture starting points in this codebase

(See `doc/specs/Kitty Graphics Protocol Support.md` for the full write-up — summarized
here for quick orientation.)

- `src/terminal/adapter/SixelParser.hpp`/`.cpp` — the closest existing analog. A new
  `KittyGraphicsParser` should mirror this class's shape and its collaboration with
  `AdaptDispatch`/`StateMachine`.
- `src/terminal/adapter/ITermDispatch.hpp` + `adaptDispatch.hpp`/`.cpp` — the dispatch
  interface pattern (see `DefineSixelImage`) to follow for new Kitty dispatch method(s).
- `src/terminal/parser/OutputStateMachineEngine.hpp`/`.cpp` — the VT state machine;
  needs a new APC dispatch path, since Kitty's protocol is APC-introduced while Sixel is
  DCS-introduced. Confirm at implementation start whether any APC handling already
  exists here.
- `src/buffer/out/ImageSlice.hpp`/`.cpp` (attached per-`Row`, see `Row.hpp`/`.cpp`) —
  worth investigating as reusable pixel-storage backing for Kitty images too, since both
  the AtlasEngine (D3D) and legacy GDI renderers already know how to paint an
  `ImageSlice` via `IRenderEngine`/`RenderEngineBase`. If this reuse holds up, it
  significantly reduces the renderer-side work — no new backend-specific compositing
  code needed, just a new producer feeding the existing rendering path.
- `doc/fuzzing.md` — this repo's fuzzing conventions; new APC parsing is untrusted-input
  surface and should go through this before being considered safe to enable by default.

## Suggested phased approach

1. **Baseline**: confirm a clean, unmodified build of this fork succeeds before any
   code changes, to isolate toolchain issues from feature work.
2. **Transmission + image store**: APC parsing for the transmit command, a
   content-addressed decoded-image cache with delete/free support.
3. **Tier 1 milestone — direct placement**: render a transmitted image at the cursor.
   First end-to-end testable checkpoint; matches WezTerm's current level of support.
4. **Tier 2 — Unicode Placeholder decode**: recognize the placeholder code point plus
   row/column diacritics plus foreground-color-encoded id in the paint path.
5. **Tier 2 — sub-image blit**: map each decoded `(row, col, imageId)` cell to the
   correct source-image sub-rectangle and render it, ideally via the `ImageSlice` path
   identified above.
6. **Reflow validation**: confirm placement survives window resize and text reflow —
   this is the entire point of Tier 2 and the hardest thing to get right.
7. **Fuzzing + security hardening**: bound decode sizes, payload sizes, and image store
   memory; run through this repo's fuzzing setup before enabling by default.
8. **Set `WT_KITTY_SUPPORTED=true`** once the above is solid.

## Constraints and things to avoid

- Do not implement Kitty's file-path-based transmission medium (`t=f`/`t=t`) initially —
  honoring an arbitrary client-specified file path is an arbitrary-file-read primitive
  for any program that can write to this terminal, including a remote one over SSH.
  Scope to the direct/escape-sequence-embedded medium (`t=d`) only.
- Do not impersonate another terminal's identity via `TERM_PROGRAM`/`WEZTERM_PANE`/
  `KITTY_WINDOW_ID` — use the additive `WT_KITTY_SUPPORTED` signal described above
  instead.
- Malformed Kitty sequences must fail closed (no-op) rather than corrupting the buffer
  or crashing.
- This is intentionally a fork-only feature; do not re-propose it upstream without a
  materially different pitch than what was already rejected (see the linked spec doc
  for the prior rejection history and a suggested narrower framing for any future
  attempt).
