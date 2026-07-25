---
author: asklar
created on: 2026-07-24
last updated: 2026-07-24
issue id: N/A — maintained in fork (github.com/asklar/terminal); see microsoft/terminal#8389 for prior upstream discussion
---

# Kitty Graphics Protocol Support

## Abstract

This spec covers adding Kitty Graphics Protocol support to this fork of Windows Terminal:
image transmission, direct (cursor-relative) placement, **and** Unicode Placeholder
("virtual") placement. The last of these is the actual target of this work, not a
stretch goal — it is what lets inline images behave like ordinary text: surviving
scroll, resize, and reflow, and participating in the terminal's normal scrollback
instead of being glued to absolute screen coordinates.

This unblocks inline image rendering (diagrams, pasted screenshots, tool output) for
any Kitty-protocol-emitting client run in this terminal — for example, terminal-based
AI coding assistants that want to show diagrams or command output inline, several of
which already implement Kitty Graphics Protocol client support today.

## Inspiration

- Several terminal-based AI coding assistants already emit real Kitty Graphics
  Protocol sequences, including Unicode Placeholder cells, for pasted screenshots and
  tool-viewed images, gated behind terminal capability detection that recognizes
  kitty, Ghostty, and WezTerm — but not Windows Terminal, because Windows Terminal
  does not implement the protocol at all. Sixel is the only raster graphics protocol
  it supports today.
- This was requested upstream in November 2020
  ([microsoft/terminal#8389](https://github.com/microsoft/terminal/issues/8389)) and
  closed **won't-fix**: the team was reluctant to add a third graphics protocol when
  none were supported at the time. A direct conversation with the current Windows
  Terminal team confirms this remains their position even now that Sixel has shipped.
- This fork exists specifically to unblock the scenario for people who want it,
  distributed separately from the official product rather than waiting on upstream
  reconsideration.

## Solution Design

### Scope tiers

- **Tier 1 — transmission + direct placement.** Image bytes are transmitted once and
  drawn at the current cursor position. This is what WezTerm already implements today,
  so it is a known-achievable bar and a natural first milestone, but it is **not**
  sufficient on its own: direct placement does not survive scroll or resize, so it only
  covers a "draw this diagram once, deliberately" interaction — not the persistent,
  reflow-safe inline-image behavior real Kitty-protocol clients expect for pasted
  screenshots and viewed image files.
- **Tier 2 — Unicode Placeholder (virtual) placement.** The required target. A
  placement is expressed as ordinary buffer cells containing the placeholder code
  point `U+10EEEE`, row/column combining diacritics encoding a logical position within
  the placement grid, and the image/placement id encoded in the cell's foreground
  color. Because this is just text-buffer content, it flows through the existing
  scroll/reflow/copy pipeline for free — the entire reason this mode exists over plain
  direct placement.

### Where this lands in the existing codebase

The closest architectural analog already in this codebase is Sixel support, so this
design mirrors its shape rather than inventing a new pattern:

- **Parser / dispatch layer** — new `KittyGraphicsParser` class in
  `src/terminal/adapter/` (new `KittyGraphicsParser.hpp`/`.cpp`), modeled directly on
  `SixelParser.hpp`/`.cpp` in the same directory: constructed with an `AdaptDispatch&`
  and `const StateMachine&`, driven by a per-character parse loop the same shape as
  `SixelParser::_parseCommandChar`/`_parseParameterChar`.
  - New pure-virtual dispatch method(s) on `ITermDispatch`
    (`src/terminal/adapter/ITermDispatch.hpp`), implemented by `AdaptDispatch`
    (`adaptDispatch.hpp`/`.cpp`), following the exact pattern of the existing
    `DefineSixelImage`.
  - Kitty's protocol is APC-introduced (`ESC _G ... ESC \`) rather than the
    DCS-introduced sequence Sixel uses, so the VT state machine
    (`src/terminal/parser/OutputStateMachineEngine.hpp`/`.cpp`) needs a new dispatch
    path for APC — needs confirming at implementation start whether any APC scaffolding
    already exists there or whether this is genuinely new to the parser.
- **Buffer / storage layer** — investigate reusing `src/buffer/out/ImageSlice.hpp`/`.cpp`
  (already attached per-`Row`, per `Row.hpp`/`.cpp`) as the pixel-storage backing for
  both placement modes. Both the AtlasEngine (D3D) backend and the legacy GDI renderer
  already know how to paint an `ImageSlice` via the `IRenderEngine`/`RenderEngineBase`
  interface (`src/renderer/inc/IRenderEngine.hpp`, `src/renderer/base/RenderEngineBase.cpp`),
  which is what Sixel output ultimately renders through. If Kitty's rasterized image
  data can be expressed as `ImageSlice`s too, this is the path that avoids needing new,
  backend-specific (D3D vs. GDI) compositing code.
- **Unicode Placeholder decode (net new — no existing analog in this codebase)** —
  decoding `U+10EEEE` + row/column diacritics + foreground-color-encoded image/placement
  id from buffer cells at paint time, mapping each decoded `(row, col, imageId)` to the
  correct sub-rectangle of the stored image, and feeding that into the `ImageSlice`-based
  compositing above rather than normal glyph rendering.
- **Image store** — content-addressed cache of decoded pixel data, plus explicit
  free/delete command support (Kitty's `a=d,d=I` etc.) per the protocol spec, so that
  well-behaved clients tracking and evicting their own shown images (e.g. bounding how
  many stay resident in a long session) round-trip correctly against this terminal.

### Capability signaling

This fork announces support via `WT_KITTY_SUPPORTED=true`, set **alongside** the stock
`WT_SESSION` marker rather than replacing or impersonating another terminal's identity
(`TERM_PROGRAM`/`WEZTERM_PANE`/`KITTY_WINDOW_ID`). Other programs in the same session
key off those existing signals for unrelated behavior, so claiming to *be* WezTerm or
kitty would risk confusing them. This is the terminal's public capability contract:
clients that want to detect Kitty Graphics Protocol support in this fork should check
for `WT_SESSION` (still genuinely Windows Terminal for everything else) plus
`WT_KITTY_SUPPORTED=true`. This flag should only be set once the corresponding tier is
actually functional, not before.

## UI/UX Design

No new user-facing settings for the initial milestone — behaves like kitty terminal's
own default-on behavior once a client asks for it. A settings toggle to disable the
feature entirely may be worth adding later, mirroring however Sixel's own on/off
behavior (if any) is currently exposed — needs checking against the settings model
during implementation.

## Capabilities

### Accessibility

Needs investigation into how Sixel-drawn regions interact with UIA/Narrator today (likely
invisible to it), and whether equivalent treatment is acceptable for Kitty placeholder
regions. The Kitty wire protocol itself has no native alt-text field, which is an
inherent protocol limitation to document rather than something this fork can solve
unilaterally.

### Security

Parsing arbitrary client-supplied image payloads is new attack surface: malformed PNG
data, decompression bombs, and oversized dimension claims all need bounds mirroring
Sixel's own constraints (`SixelParser::MAX_COLORS`, cell-size limits) and a bounded,
evicting image store sized to typical session image counts. Kitty's spec also defines
file-path-based transmission mediums (`t=f`/`t=t`, transmit via a shared temp file
path) — this design scopes the initial implementation to the direct/escape-sequence-
embedded medium (`t=d`) only, since honoring an arbitrary client-specified file path
would be an arbitrary-file-read primitive for any program (including a remote one over
SSH) that can write to the terminal.

### Reliability

Must not affect any session that never emits Kitty APC sequences — this only activates
on a new, narrow parse path no other program should be triggering. Malformed Kitty
sequences must fail closed (ignored, no buffer corruption or crash), matching how the
state machine already handles malformed sequences elsewhere.

### Compatibility

No breaking changes for anyone not opting in: `WT_KITTY_SUPPORTED` is only set once this
fork's implementation is functional, so stock behavior (e.g. a client's existing text
fallback) is preserved until then. This is intentionally a fork-only feature, not
proposed upstream again given the prior rejection and direct confirmation from the
Windows Terminal team that their position hasn't changed.

### Performance, Power, and Efficiency

Direct placement is a bounded, one-time cost per image, comparable to Sixel's existing
cost profile. Virtual (Unicode Placeholder) placement's per-cell decode-and-blit must
stay cheap enough to run every paint for every visible placeholder cell without
regressing overall render performance — worth benchmarking against the existing Sixel
paint path as a baseline, since both would ultimately feed the same `ImageSlice`/
`IRenderEngine` pipeline if that reuse holds up under implementation.

## Potential Issues

- `ImageSlice`'s Row-level storage model may not directly accommodate Kitty's "same
  image, multiple simultaneous placements at different sizes" semantics — needs
  validation once implementation starts.
- Resize/reflow correctness for placeholder-mode images is the crux of this entire
  feature and the hardest part to get right, since it's the whole reason Unicode
  Placeholder mode exists over direct placement. Needs a dedicated validation pass.
- Upstream `microsoft/terminal` ships quickly (observed 1.22 → 1.23 → 1.25 in close
  succession) — this fork needs an explicit, ideally automated rebase cadence or it will
  drift and rot the way the pre-sync fork already had (5+ years stale before this
  effort began).
- APC-sequence parsing is new, untrusted-input-facing surface in the VT state machine
  and should go through this repo's existing fuzzing setup (`doc/fuzzing.md`) before
  being considered safe to enable by default.

## Future considerations

- Once this fork proves the design out, a narrower upstream pitch (transmission +
  direct placement only, framed around the specific reflow-safety capability gap Sixel
  cannot provide, rather than "add a third protocol") could be revisited — not blocking
  for this document.
- The wire protocol is a public, documented spec, not specific to any single client, so
  any Kitty-emitting client benefits once this lands.

## Resources

- [Kitty Graphics Protocol specification](https://sw.kovidgoyal.net/kitty/graphics-protocol/)
- [microsoft/terminal#8389](https://github.com/microsoft/terminal/issues/8389) — prior
  upstream request, closed won't-fix
- Testing against real Kitty-protocol-emitting terminal applications (kitty itself,
  WezTerm) is valuable for wire-format correctness on both the client and server
  (terminal) sides of the protocol during implementation.
- This repo's existing Sixel implementation, the closest architectural analog:
  `src/terminal/adapter/SixelParser.hpp`/`.cpp`, `src/buffer/out/ImageSlice.hpp`/`.cpp`.
