// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include <consoletaeftemplates.hpp>
#include <WexTestClass.h>

#include "../adaptDispatch.hpp"
#include "../../parser/stateMachine.hpp"
#include "../../parser/OutputStateMachineEngine.hpp"
#include "../../../buffer/out/textBuffer.hpp"
#include "../../../buffer/out/KittyImageStorage.hpp"
#include "../../../renderer/inc/DummyRenderer.hpp"

using namespace WEX::TestExecution;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace Microsoft::Console::VirtualTerminal;

// This file exercises the Kitty Graphics Protocol APC dispatch path
// end-to-end: raw VT bytes -> StateMachine -> OutputStateMachineEngine ->
// AdaptDispatch -> KittyGraphicsParser -> KittyImageStorage / ImageSlice.
// It uses a small local ITerminalApi mock (KittyTestApi) rather than the
// TestGetSet mock in adapterTest.cpp, since that one hardcodes IsConPTY-era
// behaviors unrelated to this feature and isn't declared in a shared header.

namespace
{
    // RGBQUAD has no built-in equality/logging support for TAEF's
    // VERIFY_ARE_EQUAL, so pixels are compared field-by-field instead.
    void VerifyPixel(const RGBQUAD& expected, const RGBQUAD& actual)
    {
        VERIFY_ARE_EQUAL(expected.rgbBlue, actual.rgbBlue);
        VERIFY_ARE_EQUAL(expected.rgbGreen, actual.rgbGreen);
        VERIFY_ARE_EQUAL(expected.rgbRed, actual.rgbRed);
        VERIFY_ARE_EQUAL(expected.rgbReserved, actual.rgbReserved);
    }
}

namespace Microsoft::Console::VirtualTerminal
{
    class KittyGraphicsProtocolTests
    {
        TEST_CLASS(KittyGraphicsProtocolTests);

        std::unique_ptr<TerminalInput> _terminalInput;
        std::unique_ptr<StateMachine> _stateMachine;
        AdaptDispatch* _pDispatch = nullptr;
        std::unique_ptr<TextBuffer> _textBuffer;
        DummyRenderer _renderer;

        // A minimal ITerminalApi mock, local to these tests, focused on
        // exactly what the Kitty Graphics Protocol dispatch path needs
        // (buffer/cursor access, font cell size, the enabled flag, and
        // capturing APC responses for verification).
        class KittyTestApi final : public ITerminalApi
        {
        public:
            std::wstring response;
            TextBuffer* buffer = nullptr;
            StateMachine* stateMachine = nullptr;
            til::rect viewport;

            void UnknownSequence() noexcept override {}
            void ReturnResponse(const std::wstring_view r) override { response += r; }
            bool IsConPTY() const noexcept override { return false; }
            StateMachine& GetStateMachine() override { return *stateMachine; }
            BufferState GetBufferAndViewport() override
            {
                return { *buffer, viewport, true };
            }
            void SetViewportPosition(const til::point) override {}
            bool IsVtInputEnabled() const override { return false; }
            bool IsKittyGraphicsProtocolEnabled() const noexcept override { return true; }
            til::size GetFontCellSize() const override { return { 8, 16 }; }
            void SetSystemMode(const Mode, const bool) override {}
            bool GetSystemMode(const Mode) const override { return false; }
            void ReturnAnswerback() override {}
            void WarningBell() override {}
            void SetWindowTitle(const std::wstring_view) override {}
            void UseAlternateScreenBuffer(const TextAttribute&) override {}
            void UseMainScreenBuffer() override {}
            CursorType GetUserDefaultCursorStyle() const override { return CursorType::Legacy; }
            void ShowWindow(bool) override {}
            void SetCodePage(const unsigned int) override {}
            void ResetCodePage() override {}
            unsigned int GetOutputCodePage() const override { return CP_UTF8; }
            unsigned int GetInputCodePage() const override { return CP_UTF8; }
            void CopyToClipboard(const wil::zwstring_view) override {}
            void SetTaskbarProgress(const DispatchTypes::TaskbarState, const size_t) override {}
            void SetWorkingDirectory(const std::wstring_view) override {}
            void PlayMidiNote(const int, const int, const std::chrono::microseconds) override {}
            bool ResizeWindow(const til::CoordType, const til::CoordType) override { return false; }
            void NotifyBufferRotation(const int) override {}
            void NotifyShellIntegrationMark() override {}
            void InvokeCompletions(std::wstring_view, unsigned int) override {}
            void SearchMissingCommand(const std::wstring_view) override {}
            void ShowNotification(const std::wstring_view, const std::wstring_view) override {}
        };

        std::unique_ptr<KittyTestApi> _api;

        TEST_METHOD_SETUP(Setup)
        {
            _textBuffer = std::make_unique<TextBuffer>(til::size{ 80, 30 }, TextAttribute{}, 0, false, &_renderer);
            _api = std::make_unique<KittyTestApi>();
            _api->buffer = _textBuffer.get();
            _api->viewport = til::rect{ 0, 0, 80, 30 };

            _terminalInput = std::make_unique<TerminalInput>();
            Microsoft::Console::Render::RenderSettings renderSettings;
            auto dispatch = std::make_unique<AdaptDispatch>(*_api, &_renderer, renderSettings, *_terminalInput);
            _pDispatch = dispatch.get();
            auto engine = std::make_unique<OutputStateMachineEngine>(std::move(dispatch));
            _stateMachine = std::make_unique<StateMachine>(std::move(engine));
            _api->stateMachine = _stateMachine.get();
            return true;
        }

        TEST_METHOD_CLEANUP(Cleanup)
        {
            _stateMachine.reset();
            _pDispatch = nullptr;
            _api.reset();
            _textBuffer.reset();
            return true;
        }

        TEST_METHOD(TransmitAndDirectPlaceRGBA)
        {
            // 2x2 RGBA: red, green, blue, white (row-major); see the header
            // comment in KittyPlaceholder.cpp for the row-column diacritics
            // this is unrelated to -- this is plain direct (cursor) placement.
            _stateMachine->ProcessString(L"\x1b_Ga=T,f=32,s=2,v=2,i=1;/wAA/wD/AP8AAP///////w==\x1b\\");

            VERIFY_ARE_EQUAL(L"\x1b_Gi=1;OK\x1b\\", _api->response);

            const auto& row = _textBuffer->GetRowByOffset(0);
            const auto* slice = row.GetImageSlice();
            VERIFY_IS_NOT_NULL(slice);
            const auto* pixels = slice->Pixels(0);
            // Top-left pixel of the cell should be the image's (0,0): red.
            VerifyPixel(RGBQUAD{ 0, 0, 255, 255 }, pixels[0]);

            auto& storage = _textBuffer->GetKittyImageStorage();
            const auto image = storage.Find(1);
            VERIFY_IS_TRUE(static_cast<bool>(image));
            VERIFY_ARE_EQUAL(til::size(2, 2), image->pixelSize);
        }

        TEST_METHOD(TransmitThenPlaceByReference)
        {
            // 1x1 RGB: magenta.
            _stateMachine->ProcessString(L"\x1b_Ga=t,f=24,s=1,v=1,i=2;/wD/\x1b\\");
            VERIFY_ARE_EQUAL(L"\x1b_Gi=2;OK\x1b\\", _api->response);
            _api->response.clear();

            // Direct placement wasn't requested yet (a=t, not a=T), so nothing
            // should be drawn until we explicitly place it.
            VERIFY_IS_NULL(_textBuffer->GetRowByOffset(0).GetImageSlice());

            _stateMachine->ProcessString(L"\x1b_Ga=p,i=2\x1b\\");
            VERIFY_ARE_EQUAL(L"\x1b_Gi=2;OK\x1b\\", _api->response);

            const auto* slice = _textBuffer->GetRowByOffset(0).GetImageSlice();
            VERIFY_IS_NOT_NULL(slice);
            const auto* pixels = slice->Pixels(0);
            VerifyPixel(RGBQUAD{ 255, 0, 255, 255 }, pixels[0]); // magenta
        }

        TEST_METHOD(TransmitPngFormat)
        {
            // A minimal valid 1x1 PNG whose only pixel decodes to (R=0, G=255, B=0, A=127).
            _stateMachine->ProcessString(L"\x1b_Ga=t,f=100,i=3;iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==\x1b\\");
            VERIFY_ARE_EQUAL(L"\x1b_Gi=3;OK\x1b\\", _api->response);

            auto& storage = _textBuffer->GetKittyImageStorage();
            const auto image = storage.Find(3);
            VERIFY_IS_TRUE(static_cast<bool>(image));
            VERIFY_ARE_EQUAL(til::size(1, 1), image->pixelSize);
            VerifyPixel(RGBQUAD{ 0, 255, 0, 127 }, image->pixels[0]);
        }

        TEST_METHOD(VirtualPlacementGridRegistration)
        {
            _stateMachine->ProcessString(L"\x1b_Ga=p,U=1,i=4,c=3,r=2\x1b\\");
            VERIFY_ARE_EQUAL(L"\x1b_Gi=4;OK\x1b\\", _api->response);

            auto& storage = _textBuffer->GetKittyImageStorage();
            VERIFY_ARE_EQUAL(til::size(3, 2), storage.GetVirtualPlacementGrid(4));

            // A virtual placement grid registration does not itself draw anything.
            VERIFY_IS_NULL(_textBuffer->GetRowByOffset(0).GetImageSlice());
        }

        TEST_METHOD(DeleteFreesImageData)
        {
            _stateMachine->ProcessString(L"\x1b_Ga=t,f=24,s=1,v=1,i=5;/wD/\x1b\\");
            auto& storage = _textBuffer->GetKittyImageStorage();
            VERIFY_IS_TRUE(static_cast<bool>(storage.Find(5)));

            _api->response.clear();
            _stateMachine->ProcessString(L"\x1b_Ga=d,d=I,i=5\x1b\\");
            VERIFY_ARE_EQUAL(L"\x1b_Gi=5;OK\x1b\\", _api->response);
            VERIFY_IS_FALSE(static_cast<bool>(storage.Find(5)));
        }

        TEST_METHOD(UnsupportedMediumIsRejected)
        {
            // t=f (file-path medium) must be refused outright, not honored.
            _stateMachine->ProcessString(L"\x1b_Ga=t,t=f,f=100,i=6;AAAA\x1b\\");
            VERIFY_ARE_EQUAL(L"\x1b_Gi=6;EINVAL:unsupported transmission medium\x1b\\", _api->response);

            auto& storage = _textBuffer->GetKittyImageStorage();
            VERIFY_IS_FALSE(static_cast<bool>(storage.Find(6)));
        }

        TEST_METHOD(ChunkedTransmissionReassembles)
        {
            // Same 2x2 RGBA payload as TransmitAndDirectPlaceRGBA, split across
            // two escape sequences via m=1 (more coming) then m=0 (last chunk).
            _stateMachine->ProcessString(L"\x1b_Ga=T,f=32,s=2,v=2,i=7,m=1;/wAA/wD/AP8A\x1b\\");
            // Intermediate chunks are quiet by protocol convention (no ack expected mid-stream).
            VERIFY_ARE_EQUAL(std::wstring{}, _api->response);

            _stateMachine->ProcessString(L"\x1b_Ga=T,m=0;AP///////w==\x1b\\");
            VERIFY_ARE_EQUAL(L"\x1b_G;OK\x1b\\", _api->response);

            const auto* slice = _textBuffer->GetRowByOffset(0).GetImageSlice();
            VERIFY_IS_NOT_NULL(slice);
            const auto* pixels = slice->Pixels(0);
            VerifyPixel(RGBQUAD{ 0, 0, 255, 255 }, pixels[0]); // red
        }

        TEST_METHOD(NonKittyApcSequenceIsIgnoredSafely)
        {
            // Some other program's APC use (e.g. tmux-style passthrough) must
            // not crash and must not be mistaken for Kitty graphics.
            _stateMachine->ProcessString(L"\x1b_tmux;hello\x1b\\");
            VERIFY_ARE_EQUAL(std::wstring{}, _api->response);
            VERIFY_IS_NULL(_textBuffer->GetRowByOffset(0).GetImageSlice());
        }

        // This is a lightweight stand-in for real fuzzing (see doc/fuzzing.md):
        // the existing conhost-based fuzz harness can never reach this parser,
        // since the classic console always reports the capability as disabled
        // (ConhostInternalGetSet::IsKittyGraphicsProtocolEnabled). Until a
        // dedicated fuzz entry point exists, this battery of malformed/
        // adversarial inputs exercises the same fail-closed paths a fuzzer
        // would stress, directly in this test binary.
        TEST_METHOD(MalformedSequencesFailClosed)
        {
            struct
            {
                std::wstring_view name;
                std::wstring_view sequence;
            } cases[] = {
                { L"EmptySequence", L"\x1b_G\x1b\\" },
                { L"TruncatedAfterAction", L"\x1b_Ga=\x1b\\" },
                { L"TruncatedPayload", L"\x1b_Ga=t,f=32,s=100,v=100,i=1;AAAA\x1b\\" },
                { L"CorruptBase64Padding", L"\x1b_Ga=t,f=24,s=1,v=1,i=2;=A=B\x1b\\" },
                { L"HugeDeclaredDimensions", L"\x1b_Ga=t,f=32,s=999999999,v=999999999,i=3;AAAA\x1b\\" },
                { L"NegativeLikeDimensions", L"\x1b_Ga=t,f=32,s=-5,v=-5,i=4;AAAA\x1b\\" },
                { L"UnknownFormat", L"\x1b_Ga=t,f=7,i=5;AAAA\x1b\\" },
                { L"UnknownAction", L"\x1b_Ga=z,i=6\x1b\\" },
                { L"GarbageKeyValueSyntax", L"\x1b_Ga==,,;=;=,i=7\x1b\\" },
                { L"PlacementForUnknownImage", L"\x1b_Ga=p,i=999\x1b\\" },
                { L"DeleteForUnknownImage", L"\x1b_Ga=d,d=I,i=998\x1b\\" },
                { L"VirtualGridWithZeroCols", L"\x1b_Ga=p,U=1,i=8,c=0,r=5\x1b\\" },
                { L"MassiveVirtualGrid", L"\x1b_Ga=p,U=1,i=9,c=999999999,r=999999999\x1b\\" },
                { L"CorruptPng", L"\x1b_Ga=t,f=100,i=10;iVBORw0KGgoNOTAREALPNG\x1b\\" },
                { L"OnlyMoreFlagNeverFinished", L"\x1b_Ga=t,f=32,s=1,v=1,i=11,m=1;AAAA\x1b\\" },
            };

            for (const auto& c : cases)
            {
                Log::Comment(WEX::Common::String().Format(L"Case: %s", c.name.data()));
                _api->response.clear();
                // The only pass/fail criterion here is "does not throw/crash";
                // VERIFY_NO_THROW documents that intent explicitly.
                VERIFY_NO_THROW(_stateMachine->ProcessString(c.sequence));
            }

            // A sanity check that the parser is still alive and functioning
            // correctly after all of the above malformed input.
            _api->response.clear();
            _stateMachine->ProcessString(L"\x1b_Ga=t,f=24,s=1,v=1,i=100;/wD/\x1b\\");
            VERIFY_ARE_EQUAL(L"\x1b_Gi=100;OK\x1b\\", _api->response);
        }
    };
}
