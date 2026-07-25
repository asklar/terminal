/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- KittyGraphicsParser.hpp

Abstract:
- This class handles the parsing of the Kitty Graphics Protocol, transmitted
  via an APC ("Application Program Command", ESC _ ... ESC \) sequence
  beginning with the letter 'G': `ESC _ G <key>=<value>,... [; <payload>] ESC \`.
- Modeled on SixelParser's shape: constructed with an AdaptDispatch& and a
  const StateMachine&, driven by a per-character parse loop fed through the
  StringHandler returned to the state machine.
- Scope (see doc/specs/Kitty Graphics Protocol Support.md for the full
  design): embedded/direct transmission only (t=d; the file-path media t=f/t=t
  are refused, since honoring an arbitrary client-supplied path would be an
  arbitrary-file-read primitive), formats f=32 (RGBA), f=24 (RGB), and f=100
  (PNG, decoded via WIC), direct (cursor) placement, and Unicode Placeholder
  virtual-placement grid registration. Animation frames, z-index/layering,
  relative placements, and compression are out of scope.
--*/

#pragma once

#include "til.h"
#include "DispatchTypes.hpp"
#include "../buffer/out/KittyImageStorage.hpp"

namespace Microsoft::Console::VirtualTerminal
{
    class AdaptDispatch;
    class StateMachine;

    class KittyGraphicsParser
    {
    public:
        using StringHandler = std::function<bool(wchar_t)>;

        // Individual APC sequences are expected to carry a bounded (~4KB
        // base64, per the protocol's own chunking recommendation) amount of
        // data each, but we bound the total accumulated across all chunks of
        // a single transmission defensively, in case of a misbehaving or
        // malicious client that ignores the chunking recommendation.
        static constexpr size_t MaxBase64Length = 64ull * 1024 * 1024;
        // A generous but finite bound on claimed image dimensions, to fail
        // closed against bogus/adversarial size claims before we ever try to
        // allocate a pixel buffer for them.
        static constexpr til::CoordType MaxDimension = 10000;

        KittyGraphicsParser(AdaptDispatch& dispatcher, const StateMachine& stateMachine) noexcept;

        StringHandler BeginSequence();

    private:
        bool _handleChar(const wchar_t ch);
        void _finalizeKeyValue();
        void _processCommand();

        void _handleTransmit(const bool alsoDisplay);
        void _handlePlacement();
        void _handleDelete();
        void _finishTransmission();
        void _placeAtCursor(const KittyImageData& image, const std::optional<uint32_t> placementId);

        VTInt _getInt(const wchar_t key, const VTInt defaultValue) const;
        wchar_t _getChar(const wchar_t key, const wchar_t defaultValue) const;
        std::optional<uint32_t> _getOptionalUInt(const wchar_t key) const;
        void _respond(const bool ok, const std::wstring_view message = L"OK") const;

        AdaptDispatch& _dispatcher;
        const StateMachine& _stateMachine;

        enum class ParseState
        {
            ExpectG,
            Key,
            Value,
            Payload,
            Rejected
        };
        ParseState _state = ParseState::ExpectG;
        std::wstring _currentKey;
        std::wstring _currentValue;
        std::unordered_map<wchar_t, std::wstring> _kv;
        std::string _payloadBase64;

        // Cross-sequence state, so a multi-chunk transmission (m=1 ... m=0)
        // can accumulate its base64 payload across several APC sequences.
        struct PendingTransmission
        {
            uint32_t imageId = 0;
            VTInt format = 0;
            VTInt width = 0;
            VTInt height = 0;
            bool alsoDisplay = false;
            std::string base64;
        };
        std::optional<PendingTransmission> _pending;
    };
}
