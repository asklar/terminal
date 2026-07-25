// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "KittyGraphicsParser.hpp"
#include "adaptDispatch.hpp"
#include "../parser/stateMachine.hpp"
#include "../parser/ascii.hpp"
#include "../buffer/out/ImageSlice.hpp"

#include <wincodec.h>

using namespace Microsoft::Console::VirtualTerminal;
using namespace std::string_view_literals;

namespace
{
    // Decodes a base64 string into raw bytes. Non-alphabet characters
    // (including whitespace some clients may inject) are simply skipped
    // rather than treated as an error, matching common base64 decoder
    // leniency; `=` padding is honored. Returns nullopt if the decoded
    // stream is corrupt (e.g. padding in the middle of the string).
    std::optional<std::vector<uint8_t>> Base64Decode(const std::string_view text)
    {
        static constexpr auto npos = static_cast<uint8_t>(0xFF);
        static constexpr auto table = [] {
            std::array<uint8_t, 256> t{};
            t.fill(npos);
            constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (size_t i = 0; i < alphabet.size(); ++i)
            {
                t[static_cast<uint8_t>(alphabet[i])] = static_cast<uint8_t>(i);
            }
            return t;
        }();

        std::vector<uint8_t> out;
        out.reserve(text.size() / 4 * 3 + 3);

        uint32_t accumulator = 0;
        int bitsCollected = 0;
        auto sawPadding = false;
        for (const auto c : text)
        {
            const auto uc = static_cast<uint8_t>(c);
            if (c == '=')
            {
                sawPadding = true;
                continue;
            }
            if (sawPadding)
            {
                // Padding must only appear at the very end.
                return std::nullopt;
            }
            const auto value = table[uc];
            if (value == npos)
            {
                continue; // Skip whitespace/unexpected characters leniently.
            }
            accumulator = (accumulator << 6) | value;
            bitsCollected += 6;
            if (bitsCollected >= 8)
            {
                bitsCollected -= 8;
                out.push_back(static_cast<uint8_t>((accumulator >> bitsCollected) & 0xFF));
            }
        }
        return out;
    }

    // Decodes a PNG (or any other WIC-supported still-image format) from
    // raw bytes into a top-down BGRA (RGBQUAD-compatible) pixel buffer.
    bool DecodePng(const std::vector<uint8_t>& bytes, til::size& outSize, std::vector<RGBQUAD>& outPixels)
    try
    {
        static const auto coInit = wil::CoInitializeEx();

        wil::com_ptr<IStream> stream;
        THROW_IF_FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
        if (!bytes.empty())
        {
            THROW_IF_FAILED(stream->Write(bytes.data(), gsl::narrow_cast<ULONG>(bytes.size()), nullptr));
        }
        LARGE_INTEGER zero{};
        THROW_IF_FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr));

        wil::com_ptr<IWICImagingFactory> wicFactory;
        THROW_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wicFactory.put())));

        wil::com_ptr<IWICBitmapDecoder> decoder;
        THROW_IF_FAILED(wicFactory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()));

        wil::com_ptr<IWICBitmapFrameDecode> frame;
        THROW_IF_FAILED(decoder->GetFrame(0, frame.put()));

        UINT width = 0, height = 0;
        THROW_IF_FAILED(frame->GetSize(&width, &height));
        if (width == 0 || height == 0 || width > gsl::narrow_cast<UINT>(KittyGraphicsParser::MaxDimension) || height > gsl::narrow_cast<UINT>(KittyGraphicsParser::MaxDimension))
        {
            return false;
        }

        wil::com_ptr<IWICFormatConverter> converter;
        THROW_IF_FAILED(wicFactory->CreateFormatConverter(converter.put()));
        THROW_IF_FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));

        std::vector<RGBQUAD> pixels(gsl::narrow_cast<size_t>(width) * height);
        const auto stride = gsl::narrow_cast<UINT>(width * sizeof(RGBQUAD));
        THROW_IF_FAILED(converter->CopyPixels(nullptr, stride, gsl::narrow_cast<UINT>(pixels.size() * sizeof(RGBQUAD)), reinterpret_cast<BYTE*>(pixels.data())));

        outSize = { gsl::narrow_cast<til::CoordType>(width), gsl::narrow_cast<til::CoordType>(height) };
        outPixels = std::move(pixels);
        return true;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return false;
    }
}

KittyGraphicsParser::KittyGraphicsParser(AdaptDispatch& dispatcher, const StateMachine& stateMachine) noexcept :
    _dispatcher{ dispatcher },
    _stateMachine{ stateMachine }
{
}

// Routine Description:
// - Called once per APC sequence (i.e. once per "ESC _ ... ESC \\"). Resets
//   per-sequence parse state (the key=value map and payload are transient;
//   only chunked-transmission state in `_pending` survives across calls) and
//   returns the character handler the state machine will feed.
KittyGraphicsParser::StringHandler KittyGraphicsParser::BeginSequence()
{
    _state = ParseState::ExpectG;
    _currentKey.clear();
    _currentValue.clear();
    _kv.clear();
    _payloadBase64.clear();

    return [&](const auto ch) { return _handleChar(ch); };
}

// Routine Description:
// - Consumes one character of the APC sequence's content. The state machine
//   signals the end of the sequence with a final synthetic ESC character
//   (mirroring the DCS StringHandler contract).
bool KittyGraphicsParser::_handleChar(const wchar_t ch)
{
    if (ch == AsciiChars::ESC)
    {
        if (_state != ParseState::Rejected)
        {
            _finalizeKeyValue();
            _processCommand();
        }
        return true;
    }

    switch (_state)
    {
    case ParseState::ExpectG:
        _state = ch == L'G' ? ParseState::Key : ParseState::Rejected;
        return _state != ParseState::Rejected;
    case ParseState::Rejected:
        return false;
    case ParseState::Key:
        if (ch == L'=')
        {
            _state = ParseState::Value;
        }
        else if (ch == L',' || ch == L';')
        {
            // A key with no value (shouldn't normally happen); ignore it.
            _currentKey.clear();
            if (ch == L';')
            {
                _state = ParseState::Payload;
            }
        }
        else
        {
            _currentKey.push_back(ch);
        }
        return true;
    case ParseState::Value:
        if (ch == L',')
        {
            _finalizeKeyValue();
            _state = ParseState::Key;
        }
        else if (ch == L';')
        {
            _finalizeKeyValue();
            _state = ParseState::Payload;
        }
        else
        {
            _currentValue.push_back(ch);
        }
        return true;
    case ParseState::Payload:
        // The payload is base64: pure ASCII. Bound its length defensively
        // against a misbehaving/malicious client that ignores the
        // protocol's own chunking recommendation.
        if (_payloadBase64.size() >= MaxBase64Length)
        {
            _state = ParseState::Rejected;
            return false;
        }
        if (ch <= 0x7F)
        {
            _payloadBase64.push_back(static_cast<char>(ch));
        }
        return true;
    default:
        return false;
    }
}

void KittyGraphicsParser::_finalizeKeyValue()
{
    if (!_currentKey.empty())
    {
        _kv[_currentKey.front()] = _currentValue;
    }
    _currentKey.clear();
    _currentValue.clear();
}

VTInt KittyGraphicsParser::_getInt(const wchar_t key, const VTInt defaultValue) const
{
    const auto it = _kv.find(key);
    if (it == _kv.end() || it->second.empty())
    {
        return defaultValue;
    }
    try
    {
        return static_cast<VTInt>(std::stoll(it->second));
    }
    catch (...)
    {
        return defaultValue;
    }
}

wchar_t KittyGraphicsParser::_getChar(const wchar_t key, const wchar_t defaultValue) const
{
    const auto it = _kv.find(key);
    return (it == _kv.end() || it->second.empty()) ? defaultValue : it->second.front();
}

std::optional<uint32_t> KittyGraphicsParser::_getOptionalUInt(const wchar_t key) const
{
    const auto it = _kv.find(key);
    if (it == _kv.end() || it->second.empty())
    {
        return std::nullopt;
    }
    try
    {
        return static_cast<uint32_t>(std::stoull(it->second));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// Routine Description:
// - Sends the standard Kitty Graphics Protocol response, echoing back the
//   request's image/placement ids, respecting the `q=` quiet level: q=1
//   suppresses only "OK" responses, q=2 suppresses all responses.
void KittyGraphicsParser::_respond(const bool ok, const std::wstring_view message) const
{
    const auto quiet = _getInt(L'q', 0);
    if (quiet >= 2 || (quiet >= 1 && ok))
    {
        return;
    }

    std::wstring echo;
    if (const auto it = _kv.find(L'i'); it != _kv.end())
    {
        echo += L"i=";
        echo += it->second;
    }
    if (const auto it = _kv.find(L'p'); it != _kv.end())
    {
        if (!echo.empty())
        {
            echo += L',';
        }
        echo += L"p=";
        echo += it->second;
    }

    _dispatcher._ReturnApcResponse(fmt::format(FMT_COMPILE(L"G{};{}"), echo, ok ? L"OK"sv : message));
}

// Routine Description:
// - Dispatches based on the `a=` action key once a full APC sequence (header
//   and, if present, payload) has been received.
void KittyGraphicsParser::_processCommand()
{
    switch (_getChar(L'a', L't'))
    {
    case L't':
        _handleTransmit(false);
        break;
    case L'T':
        _handleTransmit(true);
        break;
    case L'p':
        _handlePlacement();
        break;
    case L'd':
        _handleDelete();
        break;
    default:
        _respond(false, L"EINVAL:unsupported action"sv);
        break;
    }
}

// Routine Description:
// - t=<transmit>/T=<transmit and display>. Only the embedded/direct medium
//   (t=d, also the default if omitted) is supported; file-path-based media
//   are refused outright rather than honored, since following an arbitrary
//   client-supplied path would be an arbitrary-file-read primitive for any
//   program that can write to this terminal.
void KittyGraphicsParser::_handleTransmit(const bool alsoDisplay)
{
    const auto medium = _getChar(L't', L'd');

    // If this sequence explicitly names a different image id than the one
    // a prior chunked (m=1) transmission is still waiting on, that earlier
    // transmission was abandoned (the client never sent its final m=0
    // chunk). Discard it rather than silently splicing this unrelated
    // sequence's payload onto it, which would corrupt both.
    if (_pending)
    {
        const auto currentId = _getOptionalUInt(L'i');
        if (currentId && *currentId != _pending->imageId)
        {
            _pending.reset();
        }
    }

    if (!_pending)
    {
        if (medium != L'd')
        {
            _respond(false, L"EINVAL:unsupported transmission medium"sv);
            return;
        }
        _pending = PendingTransmission{
            .imageId = _getOptionalUInt(L'i').value_or(0),
            .format = _getInt(L'f', 32),
            .width = _getInt(L's', 0),
            .height = _getInt(L'v', 0),
            .alsoDisplay = alsoDisplay,
        };
    }

    if (_pending->base64.size() + _payloadBase64.size() > MaxBase64Length)
    {
        _pending.reset();
        _respond(false, L"EINVAL:payload too large"sv);
        return;
    }
    _pending->base64.append(_payloadBase64);

    if (_getInt(L'm', 0) != 0)
    {
        // More chunks to come; nothing further to do until the last one.
        return;
    }

    _finishTransmission();
}

// Routine Description:
// - Finalizes a (possibly multi-chunk) transmission: base64-decodes the
//   accumulated payload, interprets it per the requested format, and stores
//   the result in the text buffer's image store.
void KittyGraphicsParser::_finishTransmission()
{
    auto pending = std::move(*_pending);
    _pending.reset();

    if (pending.width < 0 || pending.height < 0 || pending.width > MaxDimension || pending.height > MaxDimension)
    {
        _respond(false, L"EINVAL:invalid dimensions"sv);
        return;
    }

    const auto decoded = Base64Decode(pending.base64);
    if (!decoded)
    {
        _respond(false, L"EINVAL:corrupt base64 payload"sv);
        return;
    }

    til::size pixelSize;
    std::vector<RGBQUAD> pixels;

    switch (pending.format)
    {
    case 32: // RGBA, 4 bytes per pixel
    case 24: // RGB, 3 bytes per pixel
    {
        const size_t bytesPerPixel = pending.format == 32 ? 4 : 3;
        const auto expectedPixelCount = gsl::narrow_cast<size_t>(pending.width) * gsl::narrow_cast<size_t>(pending.height);
        if (pending.width <= 0 || pending.height <= 0 || decoded->size() != expectedPixelCount * bytesPerPixel)
        {
            _respond(false, L"EINVAL:pixel data size does not match dimensions"sv);
            return;
        }
        pixelSize = { pending.width, pending.height };
        pixels.resize(expectedPixelCount);
        const auto* src = decoded->data();
        for (size_t i = 0; i < expectedPixelCount; ++i)
        {
            const auto r = src[i * bytesPerPixel + 0];
            const auto g = src[i * bytesPerPixel + 1];
            const auto b = src[i * bytesPerPixel + 2];
            const auto a = bytesPerPixel == 4 ? src[i * bytesPerPixel + 3] : uint8_t{ 255 };
            pixels[i] = RGBQUAD{ b, g, r, a };
        }
        break;
    }
    case 100: // PNG, decoded via WIC
        if (!DecodePng(*decoded, pixelSize, pixels))
        {
            _respond(false, L"EINVAL:failed to decode PNG payload"sv);
            return;
        }
        break;
    default:
        _respond(false, L"EINVAL:unsupported format"sv);
        return;
    }

    auto& storage = _dispatcher._api.GetBufferAndViewport().buffer.GetKittyImageStorage();
    storage.Store(pending.imageId, pixelSize, pixels);

    if (pending.alsoDisplay)
    {
        if (const auto image = storage.Find(pending.imageId))
        {
            _placeAtCursor(*image, _getOptionalUInt(L'p'));
        }
    }

    _respond(true);
}

// Routine Description:
// - a=p: either registers a Unicode Placeholder virtual-placement grid
//   (U=1, with c=/r= columns/rows -- this does not display anything by
//   itself, it just records the grid so placeholder cells printed later as
//   ordinary text can be resolved against it), or performs a direct
//   placement of a previously transmitted image at the cursor.
void KittyGraphicsParser::_handlePlacement()
{
    const auto imageId = _getOptionalUInt(L'i').value_or(0);
    auto& storage = _dispatcher._api.GetBufferAndViewport().buffer.GetKittyImageStorage();

    if (_getInt(L'U', 0) != 0)
    {
        const auto cols = _getInt(L'c', 0);
        const auto rows = _getInt(L'r', 0);
        if (cols <= 0 || rows <= 0 || cols > MaxDimension || rows > MaxDimension)
        {
            _respond(false, L"EINVAL:invalid virtual placement grid"sv);
            return;
        }
        storage.SetVirtualPlacementGrid(imageId, { cols, rows });
        _respond(true);
        return;
    }

    const auto image = storage.Find(imageId);
    if (!image)
    {
        _respond(false, L"ENOENT:image not found"sv);
        return;
    }
    _placeAtCursor(*image, _getOptionalUInt(L'p'));
    _respond(true);
}

// Routine Description:
// - a=d: deletes a placement (d=i, keeping the image data resident for
//   reuse) or frees the image data outright (d=I). Other deletion
//   specifiers (range/animation-frame/cell-based variants) are out of scope
//   for this implementation and are accepted as harmless no-ops rather than
//   erroring, so as not to surprise well-behaved clients that send them.
void KittyGraphicsParser::_handleDelete()
{
    const auto what = _getChar(L'd', L'a');
    const auto imageId = _getOptionalUInt(L'i');

    if (imageId)
    {
        auto& storage = _dispatcher._api.GetBufferAndViewport().buffer.GetKittyImageStorage();
        if (what == L'i')
        {
            storage.SetVirtualPlacementGrid(*imageId, {});
        }
        else if (what == L'I')
        {
            storage.Delete(*imageId);
        }
    }

    _respond(true);
}

// Routine Description:
// - Renders `image` at the current cursor position by writing it into the
//   covered rows' ImageSlices, exactly the mechanism the existing Sixel
//   implementation uses -- so no renderer-backend changes are needed here.
void KittyGraphicsParser::_placeAtCursor(const KittyImageData& image, const std::optional<uint32_t> /*placementId*/)
{
    if (image.pixelSize.width <= 0 || image.pixelSize.height <= 0)
    {
        return;
    }

    const auto cellSize = _dispatcher._api.GetFontCellSize();
    if (cellSize.width <= 0 || cellSize.height <= 0)
    {
        return;
    }

    auto page = _dispatcher._pages.ActivePage();
    const auto origin = page.Cursor().GetPosition();
    const auto columnsNeeded = (image.pixelSize.width + cellSize.width - 1) / cellSize.width;
    const auto rowsNeeded = (image.pixelSize.height + cellSize.height - 1) / cellSize.height;

    for (til::CoordType r = 0; r < rowsNeeded; ++r)
    {
        const auto bufferRow = origin.y + r;
        if (bufferRow < page.Top() || bufferRow >= page.Bottom())
        {
            continue;
        }
        auto& dstRow = page.Buffer().GetMutableRowByOffset(bufferRow);
        auto* dstSlice = dstRow.GetMutableImageSlice();
        if (!dstSlice)
        {
            dstSlice = dstRow.SetImageSlice(std::make_unique<ImageSlice>(cellSize));
        }
        auto* dstIterator = dstSlice->MutablePixels(origin.x, origin.x + columnsNeeded);
        for (til::CoordType py = 0; py < cellSize.height; ++py)
        {
            const auto srcY = r * cellSize.height + py;
            for (til::CoordType px = 0; px < columnsNeeded * cellSize.width; ++px)
            {
                if (srcY < image.pixelSize.height && px < image.pixelSize.width)
                {
                    til::at(dstIterator, px) = image.pixels[gsl::narrow_cast<size_t>(srcY) * image.pixelSize.width + px];
                }
            }
            std::advance(dstIterator, dstSlice->PixelWidth());
        }
        dstSlice->BumpRevision();
    }
}
