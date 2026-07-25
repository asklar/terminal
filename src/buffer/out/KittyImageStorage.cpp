// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "KittyImageStorage.hpp"

// Routine Description:
// - Looks up a previously transmitted image by id, marking it as the most
//   recently used entry so it survives future evictions longer than colder
//   entries.
// Arguments:
// - imageId - the client-chosen id the image was transmitted with.
// Return Value:
// - A shared, read-only handle to the image data, or nullptr if not found.
std::shared_ptr<const KittyImageData> KittyImageStorage::Find(const uint32_t imageId)
{
    const auto it = _index.find(imageId);
    if (it == _index.end())
    {
        return nullptr;
    }

    // Move the found entry to the front of the LRU list (most recently used).
    _lru.splice(_lru.begin(), _lru, it->second);
    return it->second->data;
}

// Routine Description:
// - Stores (or replaces) the decoded pixel data for an image id. Per the
//   protocol spec, re-transmitting an id replaces the old data outright.
// Arguments:
// - imageId - the client-chosen id.
// - pixelSize - the pixel dimensions of the decoded image.
// - pixels - the decoded RGBQUAD pixel buffer (row-major).
// Return Value:
// - <none>
void KittyImageStorage::Store(const uint32_t imageId, til::size pixelSize, std::vector<RGBQUAD> pixels)
{
    Delete(imageId);

    auto data = std::make_shared<KittyImageData>(KittyImageData{ pixelSize, std::move(pixels) });
    _totalBytes += data->ByteSize();
    _lru.push_front(Entry{ imageId, std::move(data) });
    _index.emplace(imageId, _lru.begin());

    _evictIfNeeded();
}

// Routine Description:
// - Frees the image data associated with an id, if any (Kitty's a=d,d=I).
// Arguments:
// - imageId - the client-chosen id.
// Return Value:
// - <none>
void KittyImageStorage::Delete(const uint32_t imageId) noexcept
{
    const auto it = _index.find(imageId);
    if (it != _index.end())
    {
        _totalBytes -= it->second->data->ByteSize();
        _lru.erase(it->second);
        _index.erase(it);
    }
    _virtualPlacementGrids.erase(imageId);
}

// Routine Description:
// - Frees all stored images. Used when a session-wide reset occurs.
void KittyImageStorage::Clear() noexcept
{
    _lru.clear();
    _index.clear();
    _totalBytes = 0;
    _virtualPlacementGrids.clear();
}

void KittyImageStorage::SetVirtualPlacementGrid(const uint32_t imageId, const til::size grid)
{
    _virtualPlacementGrids[imageId] = grid;
}

til::size KittyImageStorage::GetVirtualPlacementGrid(const uint32_t imageId) const noexcept
{
    const auto it = _virtualPlacementGrids.find(imageId);
    return it != _virtualPlacementGrids.end() ? it->second : til::size{};
}

size_t KittyImageStorage::TotalBytes() const noexcept
{
    return _totalBytes;
}

void KittyImageStorage::SetMaxBytes(const size_t maxBytes) noexcept
{
    _maxBytes = maxBytes;
    _evictIfNeeded();
}

// Routine Description:
// - Evicts the least-recently-used images until the store is back under
//   budget. This bounds the store's memory use regardless of how many
//   images a long-running session transmits.
void KittyImageStorage::_evictIfNeeded()
{
    while (_totalBytes > _maxBytes && !_lru.empty())
    {
        const auto& oldest = _lru.back();
        _totalBytes -= oldest.data->ByteSize();
        _index.erase(oldest.id);
        _lru.pop_back();
    }
}
