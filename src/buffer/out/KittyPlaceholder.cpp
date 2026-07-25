// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "KittyPlaceholder.hpp"
#include "Row.hpp"
#include "TextAttribute.hpp"

using namespace std::string_view_literals;

namespace
{
    // The UTF-16 surrogate pair encoding U+10EEEE.
    constexpr wchar_t PlaceholderHighSurrogate = 0xD83B;
    constexpr wchar_t PlaceholderLowSurrogate = 0xDEEE;

    // The row/column/id-byte diacritics, in the exact order published by the
    // Kitty Graphics Protocol (rowcolumn-diacritics.txt): index N is the
    // combining mark representing the value N. This list is derived from
    // Unicode 6.0.0 combining marks of class 230 with no decomposition
    // mapping, and is a fixed part of the wire protocol -- it must match
    // exactly for interoperability with real Kitty Graphics Protocol clients.
    constexpr std::array<char32_t, 297> RowColumnDiacritics{
        0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F, 0x0346, 0x034A,
        0x034B, 0x034C, 0x0350, 0x0351, 0x0352, 0x0357, 0x035B, 0x0363, 0x0364, 0x0365,
        0x0366, 0x0367, 0x0368, 0x0369, 0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F,
        0x0483, 0x0484, 0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597,
        0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1, 0x05A8, 0x05A9,
        0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611, 0x0612, 0x0613, 0x0614, 0x0615,
        0x0616, 0x0617, 0x0657, 0x0658, 0x0659, 0x065A, 0x065B, 0x065D, 0x065E, 0x06D6,
        0x06D7, 0x06D8, 0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF, 0x06E0, 0x06E1, 0x06E2,
        0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733, 0x0735, 0x0736,
        0x073A, 0x073D, 0x073F, 0x0740, 0x0741, 0x0743, 0x0745, 0x0747, 0x0749, 0x074A,
        0x07EB, 0x07EC, 0x07ED, 0x07EE, 0x07EF, 0x07F0, 0x07F1, 0x07F3, 0x0816, 0x0817,
        0x0818, 0x0819, 0x081B, 0x081C, 0x081D, 0x081E, 0x081F, 0x0820, 0x0821, 0x0822,
        0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C, 0x082D, 0x0951,
        0x0953, 0x0954, 0x0F82, 0x0F83, 0x0F86, 0x0F87, 0x135D, 0x135E, 0x135F, 0x17DD,
        0x193A, 0x1A17, 0x1A75, 0x1A76, 0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C,
        0x1B6B, 0x1B6D, 0x1B6E, 0x1B6F, 0x1B70, 0x1B71, 0x1B72, 0x1B73, 0x1CD0, 0x1CD1,
        0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4, 0x1DC5, 0x1DC6,
        0x1DC7, 0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1, 0x1DD2, 0x1DD3, 0x1DD4, 0x1DD5,
        0x1DD6, 0x1DD7, 0x1DD8, 0x1DD9, 0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF,
        0x1DE0, 0x1DE1, 0x1DE2, 0x1DE3, 0x1DE4, 0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1,
        0x20D4, 0x20D5, 0x20D6, 0x20D7, 0x20DB, 0x20DC, 0x20E1, 0x20E7, 0x20E9, 0x20F0,
        0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2, 0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6,
        0x2DE7, 0x2DE8, 0x2DE9, 0x2DEA, 0x2DEB, 0x2DEC, 0x2DED, 0x2DEE, 0x2DEF, 0x2DF0,
        0x2DF1, 0x2DF2, 0x2DF3, 0x2DF4, 0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA,
        0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C, 0xA67D, 0xA6F0, 0xA6F1,
        0xA8E0, 0xA8E1, 0xA8E2, 0xA8E3, 0xA8E4, 0xA8E5, 0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9,
        0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED, 0xA8EE, 0xA8EF, 0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2,
        0xAAB3, 0xAAB7, 0xAAB8, 0xAABE, 0xAABF, 0xAAC1, 0xFE20, 0xFE21, 0xFE22, 0xFE23,
        0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186, 0x1D187, 0x1D188, 0x1D189,
        0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD, 0x1D242, 0x1D243, 0x1D244
    };

    // Pops a single Unicode code point off the front of `s`, handling UTF-16
    // surrogate pairs (some row/column diacritics are outside the BMP).
    char32_t _popCodepoint(std::wstring_view& s) noexcept
    {
        if (s.empty())
        {
            return 0;
        }
        const auto lead = s.front();
        if (lead >= 0xD800 && lead <= 0xDBFF && s.size() > 1)
        {
            const auto trail = s[1];
            if (trail >= 0xDC00 && trail <= 0xDFFF)
            {
                const auto cp = 0x10000u + ((static_cast<char32_t>(lead) - 0xD800u) << 10) + (static_cast<char32_t>(trail) - 0xDC00u);
                s.remove_prefix(2);
                return cp;
            }
        }
        s.remove_prefix(1);
        return lead;
    }

    struct PrevCell
    {
        bool valid = false;
        int row = 0;
        int col = 0;
        int msb = 0;
        COLORREF fg = 0;
        COLORREF ul = 0;
    };

    // Cheap bail-out check for the overwhelmingly common case: no
    // placeholder content at all in a given span of text.
    bool _containsPlaceholder(const std::wstring_view text) noexcept
    {
        for (size_t i = 0; i + 1 < text.size(); ++i)
        {
            if (text[i] == PlaceholderHighSurrogate && text[i + 1] == PlaceholderLowSurrogate)
            {
                return true;
            }
        }
        return false;
    }
}

std::optional<int> KittyPlaceholder::DiacriticValue(const char32_t ch) noexcept
{
    const auto it = std::lower_bound(RowColumnDiacritics.begin(), RowColumnDiacritics.end(), ch);
    if (it != RowColumnDiacritics.end() && *it == ch)
    {
        return static_cast<int>(std::distance(RowColumnDiacritics.begin(), it));
    }
    return std::nullopt;
}

bool KittyPlaceholder::SynchronizeRowImageSlice(ROW& row, KittyImageStorage& storage, const til::size cellSize)
{
    const auto text = row.GetText();

    // Cheap bail-out for the overwhelmingly common case: no placeholder
    // content at all in this row.
    if (!_containsPlaceholder(text))
    {
        return false;
    }

    const auto columns = row.GetReadableColumnCount();

    // Pre-size the ImageSlice's pixel buffer once for the full span of
    // placeholder columns, so per-cell blitting below doesn't repeatedly
    // reallocate as it walks left to right.
    til::CoordType minCol = columns;
    til::CoordType maxCol = -1;
    for (til::CoordType col = 0; col < columns; ++col)
    {
        const auto glyph = row.GlyphAt(col);
        if (glyph.size() >= 2 && glyph[0] == PlaceholderHighSurrogate && glyph[1] == PlaceholderLowSurrogate)
        {
            minCol = std::min(minCol, col);
            maxCol = std::max(maxCol, col);
        }
    }
    if (maxCol < minCol)
    {
        // Only the raw text matched (e.g. split across a wide-glyph boundary);
        // no actual placeholder cell found.
        return false;
    }

    auto* dstSlice = row.GetMutableImageSlice();
    if (!dstSlice)
    {
        dstSlice = row.SetImageSlice(std::make_unique<ImageSlice>(cellSize));
    }
    // Ensure the buffer covers the full range up front.
    std::ignore = dstSlice->MutablePixels(minCol, maxCol + 1);

    PrevCell prev;
    for (til::CoordType col = 0; col < columns; ++col)
    {
        const auto glyph = row.GlyphAt(col);
        if (glyph.size() < 2 || glyph[0] != PlaceholderHighSurrogate || glyph[1] != PlaceholderLowSurrogate)
        {
            prev = {};
            continue;
        }

        const auto attr = row.GetAttrByColumn(col);
        const auto fgColor = attr.GetForeground();
        const auto ulColor = attr.GetUnderlineColor();

        uint32_t baseId = 0;
        COLORREF fgForComparison = 0;
        if (fgColor.IsRgb())
        {
            fgForComparison = fgColor.GetRGB();
            baseId = static_cast<uint32_t>(fgForComparison) & 0xFFFFFFu;
        }
        else if (fgColor.IsIndex256() || fgColor.IsIndex16())
        {
            fgForComparison = RGB(fgColor.GetIndex(), 0, 0);
            baseId = fgColor.GetIndex();
        }
        else
        {
            // No usable id-carrying foreground color; can't resolve an image.
            prev = {};
            continue;
        }
        const auto ulForComparison = ulColor.IsRgb() ? ulColor.GetRGB() : static_cast<COLORREF>(0);

        // Decode up to 3 diacritics (row, column, most-significant id byte)
        // following the base placeholder code point.
        auto remaining = glyph;
        std::ignore = _popCodepoint(remaining); // consume the base placeholder itself
        std::optional<int> rowValue, colValue, msbValue;
        for (auto i = 0; i < 3 && !remaining.empty(); ++i)
        {
            const auto value = KittyPlaceholder::DiacriticValue(_popCodepoint(remaining));
            if (!value)
            {
                break;
            }
            (i == 0 ? rowValue : (i == 1 ? colValue : msbValue)) = value;
        }

        const auto sameColors = prev.valid && prev.fg == fgForComparison && prev.ul == ulForComparison;
        int decodedRow, decodedCol, decodedMsb;
        if (!rowValue && !colValue && !msbValue && sameColors)
        {
            decodedRow = prev.row;
            decodedCol = prev.col + 1;
            decodedMsb = prev.msb;
        }
        else if (rowValue && !colValue && !msbValue && sameColors && prev.row == *rowValue)
        {
            decodedRow = *rowValue;
            decodedCol = prev.col + 1;
            decodedMsb = prev.msb;
        }
        else if (rowValue && colValue && !msbValue && sameColors && prev.row == *rowValue && prev.col + 1 == *colValue)
        {
            decodedRow = *rowValue;
            decodedCol = *colValue;
            decodedMsb = prev.msb;
        }
        else
        {
            decodedRow = rowValue.value_or(0);
            decodedCol = colValue.value_or(0);
            decodedMsb = msbValue.value_or(0);
        }

        prev = PrevCell{ true, decodedRow, decodedCol, decodedMsb, fgForComparison, ulForComparison };

        const auto imageId = (static_cast<uint32_t>(decodedMsb) << 24) | baseId;
        const auto grid = storage.GetVirtualPlacementGrid(imageId);
        const auto image = storage.Find(imageId);
        if (grid.width <= 0 || grid.height <= 0 || !image || image->pixelSize.width <= 0 || image->pixelSize.height <= 0)
        {
            // Can't resolve this cell's image yet (not transmitted, or no
            // virtual placement grid registered) -- leave its pixels as-is
            // and let the placeholder glyph (typically invisible) show
            // through instead of corrupting anything.
            continue;
        }

        const auto srcX0 = static_cast<til::CoordType>(static_cast<int64_t>(image->pixelSize.width) * decodedCol / grid.width);
        const auto srcX1 = static_cast<til::CoordType>(static_cast<int64_t>(image->pixelSize.width) * (decodedCol + 1) / grid.width);
        const auto srcY0 = static_cast<til::CoordType>(static_cast<int64_t>(image->pixelSize.height) * decodedRow / grid.height);
        const auto srcY1 = static_cast<til::CoordType>(static_cast<int64_t>(image->pixelSize.height) * (decodedRow + 1) / grid.height);
        const auto srcWidth = std::max<til::CoordType>(1, srcX1 - srcX0);
        const auto srcHeight = std::max<til::CoordType>(1, srcY1 - srcY0);

        auto* dstIterator = dstSlice->MutablePixels(col, col + 1);
        for (til::CoordType y = 0; y < cellSize.height; ++y)
        {
            const auto srcY = std::clamp<til::CoordType>(srcY0 + (y * srcHeight / std::max<til::CoordType>(1, cellSize.height)), 0, image->pixelSize.height - 1);
            for (til::CoordType x = 0; x < cellSize.width; ++x)
            {
                const auto srcX = std::clamp<til::CoordType>(srcX0 + (x * srcWidth / std::max<til::CoordType>(1, cellSize.width)), 0, image->pixelSize.width - 1);
                til::at(dstIterator, x) = image->pixels[static_cast<size_t>(srcY) * image->pixelSize.width + srcX];
            }
            std::advance(dstIterator, dstSlice->PixelWidth());
        }
    }

    dstSlice->BumpRevision();
    return true;
}

// Routine Description:
// - See header comment. Walks `text`, copying everything through unchanged
//   except placeholder sequences (base code point + up to 3 trailing
//   diacritics), which are dropped entirely.
std::wstring KittyPlaceholder::StripPlaceholders(const std::wstring_view text)
{
    if (!_containsPlaceholder(text))
    {
        return std::wstring{ text };
    }

    std::wstring out;
    out.reserve(text.size());

    auto remaining = text;
    while (!remaining.empty())
    {
        if (remaining.size() >= 2 && remaining[0] == PlaceholderHighSurrogate && remaining[1] == PlaceholderLowSurrogate)
        {
            remaining.remove_prefix(2);
            // Consume up to 3 trailing diacritics (each 1 or 2 wchar_t).
            for (auto i = 0; i < 3 && !remaining.empty(); ++i)
            {
                auto probe = remaining;
                const auto cp = _popCodepoint(probe);
                if (!DiacriticValue(cp))
                {
                    break;
                }
                remaining = probe;
            }
        }
        else
        {
            out.push_back(remaining.front());
            remaining.remove_prefix(1);
        }
    }
    return out;
}
