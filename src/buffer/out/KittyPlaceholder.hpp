/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- KittyPlaceholder.hpp

Abstract:
- Implements decoding of Kitty Graphics Protocol "Unicode Placeholder" cells.
- A placement made this way is just ordinary text: the placeholder code
  point U+10EEEE, followed by up to three combining diacritics that encode
  the cell's (row, column) position within the placement grid plus the most
  significant byte of the image id, with the low 24 (or 8) bits of the id
  encoded in the cell's foreground color. Because this is ordinary buffer
  text, it already survives scrolling, resizing, and reflow via the normal
  text buffer machinery -- no new storage is required for the placement
  itself.
- What *is* new here is recognizing this pattern at paint time and
  translating it into the same per-row ImageSlice mechanism the Sixel
  implementation already uses, so painting requires no renderer-backend
  changes: AtlasEngine and the legacy GDI engine already know how to paint
  an ImageSlice.
--*/

#pragma once

#include "til.h"
#include "KittyImageStorage.hpp"

class ROW;

namespace KittyPlaceholder
{
    // U+10EEEE, the Private Use Area code point reserved by the Kitty
    // Graphics Protocol as an image placeholder.
    constexpr char32_t Codepoint = 0x10EEEEu;

    // Returns the row/column/id-byte value (0-296) encoded by a single
    // "row-column diacritic" combining mark, or nullopt if `ch` is not one
    // of the recognized diacritics.
    std::optional<int> DiacriticValue(const char32_t ch) noexcept;

    // Scans `row` for placeholder cells and, if any are found, (re)builds
    // its ImageSlice from the image(s) they reference so the existing
    // ImageSlice-painting path renders the correct sub-rectangles. This is
    // safe to call on every row on every paint: rows without any
    // placeholder content bail out immediately after a cheap substring
    // search.
    // Arguments:
    // - row - the row to scan and (if applicable) attach/update an ImageSlice on.
    // - storage - the image store to resolve referenced image ids against.
    // - cellSize - the current font's cell size, in pixels.
    // Return Value:
    // - true if the row contains any placeholder cells (whether or not the
    //   referenced images could actually be resolved).
    bool SynchronizeRowImageSlice(ROW& row, KittyImageStorage& storage, const til::size cellSize);

    // Strips any Kitty Graphics Protocol Unicode Placeholder sequences
    // (the base code point plus its 0-3 trailing row/column/id diacritics)
    // out of `text`, so plain-text consumers -- screen readers/UIA and
    // clipboard copy alike -- don't see private-use code points and
    // meaningless combining marks where an image is displayed. Text with no
    // placeholder content is returned unchanged (cheaply, via the same
    // substring bail-out SynchronizeRowImageSlice uses).
    std::wstring StripPlaceholders(const std::wstring_view text);
}
