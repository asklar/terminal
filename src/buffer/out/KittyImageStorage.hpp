/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- KittyImageStorage.hpp

Abstract:
- This serves as the content-addressed store for images transmitted via the
  Kitty Graphics Protocol. Images are decoded once (see KittyGraphicsParser)
  and kept here, indexed by the client-chosen image id, so that later
  placements (direct or via Unicode Placeholder cells) can reference the
  pixel data without re-transmitting or re-decoding it.
- The store is bounded by a byte budget (default 256 MiB) and evicts the
  least-recently-used image first, so a long-running session with many
  transmitted images cannot grow the store without bound. This mirrors the
  "bounded, evicting image store" requirement from the Kitty Graphics
  Protocol design spec.
--*/

#pragma once

#include "til.h"
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

// The decoded, ready-to-blit pixel data for a single transmitted image.
struct KittyImageData
{
    til::size pixelSize;
    // Row-major RGBQUAD pixels, pixelSize.width * pixelSize.height entries.
    std::vector<RGBQUAD> pixels;

    size_t ByteSize() const noexcept
    {
        return pixels.size() * sizeof(RGBQUAD);
    }
};

class KittyImageStorage
{
public:
    // Per the design spec, the decoded-image cache defaults to a 256 MiB
    // budget with LRU eviction.
    static constexpr size_t DefaultMaxBytes = 256ull * 1024 * 1024;

    KittyImageStorage() = default;

    std::shared_ptr<const KittyImageData> Find(const uint32_t imageId);
    void Store(const uint32_t imageId, til::size pixelSize, std::vector<RGBQUAD> pixels);
    void Delete(const uint32_t imageId) noexcept;
    void Clear() noexcept;

    // The row/column grid dimensions for an image's Unicode Placeholder
    // ("virtual") placement, set via `a=p,U=1,i=<id>,c=<cols>,r=<rows>` and
    // later consulted at paint time to know what fraction of the image each
    // placeholder cell represents. {0, 0} means "not set".
    void SetVirtualPlacementGrid(const uint32_t imageId, const til::size grid);
    til::size GetVirtualPlacementGrid(const uint32_t imageId) const noexcept;

    size_t TotalBytes() const noexcept;
    void SetMaxBytes(const size_t maxBytes) noexcept;

private:
    struct Entry
    {
        uint32_t id;
        std::shared_ptr<const KittyImageData> data;
    };

    void _evictIfNeeded();

    // The front of the list is the most-recently-used entry.
    std::list<Entry> _lru;
    std::unordered_map<uint32_t, std::list<Entry>::iterator> _index;
    std::unordered_map<uint32_t, til::size> _virtualPlacementGrids;
    size_t _totalBytes = 0;
    size_t _maxBytes = DefaultMaxBytes;
};
