/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLITerminalUnitTests.cpp

Abstract:

    Unit tests for OutputChannel, InputChannel, and Terminal.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "APICompat.h"
#include "InputChannel.h"
#include "OutputChannel.h"
#include "DiagnosticCallback.h"
#include "Terminal.h"
#include "WSLCDiagnostics.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::common::vt;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLITerminalUnitTests {

// Dual-pipe Terminal so stdout and stderr can be asserted independently.
struct SplitCaptureTerminal
{
    CapturePipe outPipe;
    CapturePipe errPipe;
    Terminal terminal;

    explicit SplitCaptureTerminal(bool vtEnabled = false) : terminal(outPipe.file(), vtEnabled, errPipe.file(), vtEnabled)
    {
    }
};

// Terminal wired with a preloaded input pipe plus split output capture, so prompt
// input and the label/newline it writes can be asserted together.
struct InputCaptureTerminal
{
    CapturePipe outPipe;
    CapturePipe errPipe;
    InputPipe inPipe;
    Terminal terminal;

    explicit InputCaptureTerminal(const std::wstring& input, bool interactive = false) :
        inPipe(input), terminal(outPipe.file(), false, errPipe.file(), false, inPipe.file(), interactive)
    {
    }
};

struct TestDiagnosticCallback : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDiagnosticCallback>
{
    HRESULT GetEnabledLevels(WSLCDiagnosticLevel* levels) override
    {
        RETURN_HR_IF_NULL(E_POINTER, levels);
        ++EnabledLevelsQueryCount;
        RETURN_IF_FAILED(GetEnabledLevelsResult);
        *levels = EnabledLevels;
        return S_OK;
    }

    HRESULT OnDiagnostic(const WSLCDiagnosticEvent* event) override
    {
        RETURN_HR_IF_NULL(E_POINTER, event);
        ++DiagnosticCount;
        LastLevel = event->Level;
        LastCode = event->Code == nullptr ? "" : event->Code;
        LastMessage = event->Message == nullptr ? L"" : event->Message;
        return OnDiagnosticResult;
    }

    WSLCDiagnosticLevel EnabledLevels = WSLCDiagnosticLevelNone;
    HRESULT GetEnabledLevelsResult = S_OK;
    HRESULT OnDiagnosticResult = S_OK;
    size_t EnabledLevelsQueryCount = 0;
    size_t DiagnosticCount = 0;
    WSLCDiagnosticLevel LastLevel = WSLCDiagnosticLevelNone;
    std::string LastCode;
    std::wstring LastMessage;
};

struct TestCompatWarningCallback
    : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCCompatWarningCallback>
{
    HRESULT OnWarning(LPCWSTR message) override
    {
        RETURN_HR_IF_NULL(E_INVALIDARG, message);
        ++WarningCount;
        LastMessage = message;
        return S_OK;
    }

    size_t WarningCount = 0;
    std::wstring LastMessage;
};

WSLCDiagnosticEvent DiagnosticEvent(WSLCDiagnosticLevel level, LPCSTR code, LPCWSTR message = nullptr)
{
    WSLCDiagnosticEvent event{};
    event.SchemaVersion = WSLC_DIAGNOSTIC_SCHEMA_VERSION;
    event.Level = level;
    event.Code = code;
    event.Message = message;
    return event;
}

void VerifyDebugLine(std::wstring_view output, std::wstring_view body)
{
    constexpr std::wstring_view prefix = L"[debug] ";
    constexpr size_t timestampLength = 12;
    constexpr size_t labelLength = prefix.size() + timestampLength + 1;

    VERIFY_IS_TRUE(output.size() >= labelLength);
    if (output.size() < labelLength)
    {
        return;
    }

    VERIFY_ARE_EQUAL(std::wstring{prefix}, std::wstring{output.substr(0, prefix.size())});

    const auto timestamp = output.substr(prefix.size(), timestampLength);
    for (size_t index = 0; index < timestamp.size(); ++index)
    {
        if (index == 2 || index == 5)
        {
            VERIFY_ARE_EQUAL(L':', timestamp[index]);
        }
        else if (index == 8)
        {
            VERIFY_ARE_EQUAL(L'.', timestamp[index]);
        }
        else
        {
            VERIFY_IS_TRUE(timestamp[index] >= L'0' && timestamp[index] <= L'9');
        }
    }

    VERIFY_ARE_EQUAL(L' ', output[prefix.size() + timestampLength]);
    VERIFY_ARE_EQUAL(std::wstring{body}, std::wstring{output.substr(labelLength)});
}

class WSLCCLITerminalUnitTests
{
    WSLC_TEST_CLASS(WSLCCLITerminalUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(OutputChannel_WriteStringWritesText)
    {
        CapturePipe pipe;
        const OutputChannel channel{pipe.file(), false};
        channel.WriteString(L"hello");
        VERIFY_ARE_EQUAL(std::wstring{L"hello"}, pipe.captured());
    }

    TEST_METHOD(OutputChannel_WriteStringIsNoOpOnEmpty)
    {
        CapturePipe pipe;
        const OutputChannel channel{pipe.file(), false};
        channel.WriteString(L"");
        VERIFY_ARE_EQUAL(std::wstring{L""}, pipe.captured());
    }

    TEST_METHOD(OutputChannel_FromHandleFallsBackToFileForNonConsole)
    {
        CapturePipe pipe;
        const OutputChannel channel{INVALID_HANDLE_VALUE, pipe.file()};
        channel.WriteString(L"fallback");
        VERIFY_ARE_EQUAL(std::wstring{L"fallback"}, pipe.captured());
        VERIFY_IS_FALSE(channel.GetConsoleWidth().has_value());
    }

    TEST_METHOD(OutputChannel_GetConsoleWidth_FileChannelReturnsNullopt)
    {
        CapturePipe pipe;
        const OutputChannel channel{pipe.file(), false};
        VERIFY_IS_FALSE(channel.GetConsoleWidth().has_value());
    }

    TEST_METHOD(Terminal_WriteEmitsExactText)
    {
        CaptureTerminal cap;
        cap.terminal.Output(L"hello\n");
        VERIFY_ARE_EQUAL(std::wstring{L"hello\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_WriteWithoutNewline)
    {
        CaptureTerminal cap;
        cap.terminal.Write(Terminal::Level::Output, L"hello");
        VERIFY_ARE_EQUAL(std::wstring{L"hello"}, cap.captured());
    }

    TEST_METHOD(Terminal_FormatStringSubstitutesArgs)
    {
        CaptureTerminal cap;
        cap.terminal.Output(L"value={}, name={}\n", 42, L"alice");
        VERIFY_ARE_EQUAL(std::wstring{L"value=42, name=alice\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_PlainStringNeedsNoArgs)
    {
        CaptureTerminal cap;
        cap.terminal.Output(L"plain literal\n");
        VERIFY_ARE_EQUAL(std::wstring{L"plain literal\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_SequenceEmittedWhenVTEnabled)
    {
        CaptureTerminal cap{/*vtEnabled*/ true};
        cap.terminal.Output(L"{}highlighted{}\n", Format::Fg::BrightYellow, Format::Default);

        const auto result = cap.captured();
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"highlighted"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(Format::Fg::BrightYellow.Get()));
    }

    TEST_METHOD(Terminal_SequenceStrippedWhenVTDisabled)
    {
        CaptureTerminal cap{/*vtEnabled*/ false};
        cap.terminal.Output(L"{}plain{}\n", Format::Fg::BrightYellow, Format::Default);
        VERIFY_ARE_EQUAL(std::wstring{L"plain\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_ColorSequenceStrippedWhenNoColor)
    {
        CaptureTerminal cap{/*vtEnabled*/ true};
        cap.terminal.SetNoColor(true);

        // Color sequence (SGR) stripped; cursor moves (non-color) still pass.
        cap.terminal.Output(L"{}{}plain{}\n", Cursor::Up(1), Format::Fg::BrightRed, Format::Default);

        const auto result = cap.captured();
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(Format::Fg::BrightRed.Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(Cursor::Up(1).Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"plain"));
    }

    TEST_METHOD(Terminal_ConstructedSequenceHandledLikeSequence)
    {
        CaptureTerminal cap{/*vtEnabled*/ true};
        const auto cursor = Cursor::Up(3);
        cap.terminal.Output(L"{}done\n", cursor);

        const auto result = cap.captured();
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(cursor.Get()));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"done"));
    }

    TEST_METHOD(Terminal_LevelColorWrapsOutputWhenVTEnabled)
    {
        CaptureTerminal cap{/*vtEnabled*/ true};

        cap.terminal.Output(L"starting\n");
        cap.terminal.Info(L"pulling\n");
        cap.terminal.Warn(L"careful\n");
        cap.terminal.Error(L"failed\n");

        const std::wstring def{Format::Default.Get()};
        const std::wstring yellow{Format::Fg::BrightYellow.Get()};
        const std::wstring red{Format::Fg::BrightRed.Get()};

        const auto expected = std::wstring{L"starting\npulling\n"} + yellow + L"careful\n" + def + red + L"failed\n" + def;

        VERIFY_ARE_EQUAL(expected, cap.captured());
    }

    TEST_METHOD(Terminal_LevelColorSuppressedWhenVTDisabled)
    {
        CaptureTerminal cap{/*vtEnabled*/ false};
        cap.terminal.Error(L"failed\n");
        VERIFY_ARE_EQUAL(std::wstring{L"failed\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_LevelColorSuppressedWhenNoColor)
    {
        CaptureTerminal cap{/*vtEnabled*/ true};
        cap.terminal.SetNoColor(true);
        cap.terminal.Warn(L"careful\n");
        VERIFY_ARE_EQUAL(std::wstring{L"careful\n"}, cap.captured());
    }

    TEST_METHOD(Terminal_RoutingByLevel)
    {
        SplitCaptureTerminal cap;

        cap.terminal.Output(L"output text\n");
        cap.terminal.Info(L"info text\n");
        cap.terminal.Debug(L"hidden debug text\n");
        cap.terminal.Warn(L"warn text\n");
        cap.terminal.Error(L"error text\n");

        VERIFY_ARE_EQUAL(std::wstring{L"output text\n"}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L"info text\nwarn text\nerror text\n"}, cap.errPipe.captured());
    }

    TEST_METHOD(Terminal_DebugOutputIsFilteredAndRoutedToStderr)
    {
        {
            SplitCaptureTerminal cap;

            VERIFY_IS_FALSE(cap.terminal.IsDebugEnabled());
            cap.terminal.Debug(L"hidden\n");
            VERIFY_ARE_EQUAL(std::wstring{}, cap.errPipe.captured());
        }

        {
            SplitCaptureTerminal cap;
            cap.terminal.SetDebugEnabled(true);
            VERIFY_IS_TRUE(cap.terminal.IsDebugEnabled());
            cap.terminal.Debug(L"value={}\n", 42);

            VERIFY_ARE_EQUAL(std::wstring{}, cap.outPipe.captured());
            VerifyDebugLine(cap.errPipe.captured(), L"value=42\n");
        }

        {
            SplitCaptureTerminal cap{/*vt*/ true};
            cap.terminal.SetDebugEnabled(true);
            cap.terminal.Debug(L"value={}\n", 42);

            const auto output = cap.errPipe.captured();
            const auto prefix = Format::Dim.Get();
            const auto suffix = Format::Default.Get();
            const bool hasFormatting =
                output.size() >= prefix.size() + suffix.size() && output.starts_with(prefix) && output.ends_with(suffix);
            VERIFY_IS_TRUE(hasFormatting);
            if (hasFormatting)
            {
                VerifyDebugLine(
                    std::wstring_view{output}.substr(prefix.size(), output.size() - prefix.size() - suffix.size()), L"value=42\n");
            }
        }

        {
            SplitCaptureTerminal cap{/*vt*/ true};
            cap.terminal.SetNoColor(true);
            cap.terminal.SetDebugEnabled(true);
            cap.terminal.Debug(L"value={}\n", 42);

            VerifyDebugLine(cap.errPipe.captured(), L"value=42\n");
        }
    }

    TEST_METHOD(DiagnosticCallback_AdvertisesAndRoutesEnabledLevels)
    {
        {
            SplitCaptureTerminal cap;
            services::DiagnosticCallback callback(cap.terminal);

            WSLCDiagnosticLevel levels{};
            VERIFY_ARE_EQUAL(E_POINTER, callback.GetEnabledLevels(nullptr));
            VERIFY_SUCCEEDED(callback.GetEnabledLevels(&levels));
            VERIFY_IS_FALSE(WI_IsFlagSet(levels, WSLCDiagnosticLevelDebug));
            VERIFY_IS_TRUE(WI_IsFlagSet(levels, WSLCDiagnosticLevelInformation));
            VERIFY_IS_TRUE(WI_IsFlagSet(levels, WSLCDiagnosticLevelWarning));
            VERIFY_IS_TRUE(WI_IsFlagSet(levels, WSLCDiagnosticLevelError));

            const auto debugEvent = DiagnosticEvent(WSLCDiagnosticLevelDebug, WSLC_DIAG_CODE_SESSION_RESOLUTION_STARTED);
            const auto informationEvent = DiagnosticEvent(WSLCDiagnosticLevelInformation, "information", L"information\n");
            const auto warningEvent = DiagnosticEvent(WSLCDiagnosticLevelWarning, WSLC_DIAG_CODE_USER_WARNING, L"warning\n");
            const auto errorEvent = DiagnosticEvent(WSLCDiagnosticLevelError, "error", L"error\n");
            const auto fallbackEvent = DiagnosticEvent(WSLCDiagnosticLevelInformation, "information-without-message");
            const auto invalidLevelEvent = DiagnosticEvent(
                static_cast<WSLCDiagnosticLevel>(WSLCDiagnosticLevelDebug | WSLCDiagnosticLevelInformation), "invalid-level");
            auto invalidVersionEvent = DiagnosticEvent(WSLCDiagnosticLevelInformation, "invalid-version");
            ++invalidVersionEvent.SchemaVersion;
            const auto missingCodeEvent = DiagnosticEvent(WSLCDiagnosticLevelInformation, nullptr);

            VERIFY_ARE_EQUAL(E_POINTER, callback.OnDiagnostic(nullptr));
            VERIFY_SUCCEEDED(callback.OnDiagnostic(&debugEvent));
            VERIFY_SUCCEEDED(callback.OnDiagnostic(&informationEvent));
            VERIFY_SUCCEEDED(callback.OnDiagnostic(&warningEvent));
            VERIFY_SUCCEEDED(callback.OnDiagnostic(&errorEvent));
            VERIFY_SUCCEEDED(callback.OnDiagnostic(&fallbackEvent));
            VERIFY_ARE_EQUAL(E_INVALIDARG, callback.OnDiagnostic(&invalidLevelEvent));
            VERIFY_ARE_EQUAL(E_INVALIDARG, callback.OnDiagnostic(&invalidVersionEvent));
            VERIFY_ARE_EQUAL(E_INVALIDARG, callback.OnDiagnostic(&missingCodeEvent));
            VERIFY_ARE_EQUAL(std::wstring{L"information\nwarning\nerror\ninformation-without-message\n"}, cap.errPipe.captured());
        }

        {
            SplitCaptureTerminal cap;
            cap.terminal.SetDebugEnabled(true);
            services::DiagnosticCallback callback(cap.terminal);

            WSLCDiagnosticLevel levels{};
            VERIFY_SUCCEEDED(callback.GetEnabledLevels(&levels));
            VERIFY_IS_TRUE(WI_IsFlagSet(levels, WSLCDiagnosticLevelDebug));
            const auto sessionEvent =
                DiagnosticEvent(WSLCDiagnosticLevelDebug, WSLC_DIAG_CODE_SESSION_RESOLUTION_STARTED, L"Name: test");
            const auto futureEvent = DiagnosticEvent(WSLCDiagnosticLevelDebug, "future-event");
            VERIFY_SUCCEEDED(callback.OnDiagnostic(&sessionEvent));
            VERIFY_SUCCEEDED(callback.OnDiagnostic(&futureEvent));

            const auto output = cap.errPipe.captured();
            VERIFY_ARE_NOT_EQUAL(std::wstring::npos, output.find(L" [session-resolution-started] Name: test\n"));
            VERIFY_ARE_NOT_EQUAL(std::wstring::npos, output.find(L" [future-event]\n"));
        }
    }

    TEST_METHOD(DiagnosticCallback_PreservesSdkWarningContract)
    {
        constexpr GUID expectedIid{0x290C58A1, 0x328C, 0x4E90, {0xB0, 0x9B, 0x0A, 0x9D, 0x32, 0xC4, 0x00, 0x74}};
        VERIFY_IS_TRUE(IsEqualGUID(expectedIid, __uuidof(IWSLCCompatWarningCallback)));

        auto warningCallback = Microsoft::WRL::Make<TestCompatWarningCallback>();
        const auto diagnosticCallback = wsl::windows::common::apicompat::Convert(warningCallback.Get());
        VERIFY_IS_NOT_NULL(diagnosticCallback.Get());

        WSLCDiagnosticLevel levels{};
        VERIFY_SUCCEEDED(diagnosticCallback->GetEnabledLevels(&levels));
        VERIFY_ARE_EQUAL(WSLCDiagnosticLevelWarning, levels);

        const auto warningEvent = DiagnosticEvent(WSLCDiagnosticLevelWarning, WSLC_DIAG_CODE_USER_WARNING, L"sdk warning\n");
        VERIFY_SUCCEEDED(diagnosticCallback->OnDiagnostic(&warningEvent));
        VERIFY_ARE_EQUAL(size_t{1}, warningCallback->WarningCount);
        VERIFY_ARE_EQUAL(std::wstring{L"sdk warning\n"}, warningCallback->LastMessage);

        const auto informationEvent =
            DiagnosticEvent(WSLCDiagnosticLevelInformation, "information", L"not supported by the SDK callback\n");
        const auto missingMessageEvent = DiagnosticEvent(WSLCDiagnosticLevelWarning, WSLC_DIAG_CODE_USER_WARNING);
        VERIFY_ARE_EQUAL(E_INVALIDARG, diagnosticCallback->OnDiagnostic(&informationEvent));
        VERIFY_ARE_EQUAL(E_INVALIDARG, diagnosticCallback->OnDiagnostic(&missingMessageEvent));
        VERIFY_ARE_EQUAL(size_t{1}, warningCallback->WarningCount);
    }

    TEST_METHOD(DiagnosticHelpers_FilterAndIgnoreCallbackFailures)
    {
        TestDiagnosticCallback callback;
        callback.EnabledLevels = WSLCDiagnosticLevelDebug | WSLCDiagnosticLevelWarning;

        const auto enabledLevels = wsl::windows::wslc::diagnostics::GetEnabledLevels(&callback);
        VERIFY_ARE_EQUAL(callback.EnabledLevels, enabledLevels);

        wsl::windows::wslc::diagnostics::Report(
            &callback, enabledLevels, WSLCDiagnosticLevelInformation, "information", L"filtered");
        VERIFY_ARE_EQUAL(size_t{0}, callback.DiagnosticCount);

        callback.OnDiagnosticResult = E_FAIL;
        wsl::windows::wslc::diagnostics::Report(
            &callback, enabledLevels, WSLCDiagnosticLevelWarning, WSLC_DIAG_CODE_USER_WARNING, L"warning");
        VERIFY_ARE_EQUAL(size_t{1}, callback.DiagnosticCount);
        VERIFY_ARE_EQUAL(WSLCDiagnosticLevelWarning, callback.LastLevel);
        VERIFY_ARE_EQUAL(std::string{WSLC_DIAG_CODE_USER_WARNING}, callback.LastCode);
        VERIFY_ARE_EQUAL(std::wstring{L"warning"}, callback.LastMessage);

        callback.EnabledLevels = static_cast<WSLCDiagnosticLevel>(0x10);
        VERIFY_ARE_EQUAL(WSLCDiagnosticLevelNone, wsl::windows::wslc::diagnostics::GetEnabledLevels(&callback));

        callback.GetEnabledLevelsResult = E_FAIL;
        VERIFY_ARE_EQUAL(WSLCDiagnosticLevelNone, wsl::windows::wslc::diagnostics::GetEnabledLevels(&callback));
    }

    TEST_METHOD(DiagnosticHelpers_SanitizeString)
    {
        using wsl::windows::wslc::diagnostics::SanitizeString;
        using wsl::windows::wslc::diagnostics::SanitizeUserName;

        VERIFY_ARE_EQUAL(
            std::wstring{L"<user> used <token>; <user>"},
            SanitizeString(L"Alice used token-123; ALICE", {{L"alice", L"<user>"}, {L"token-123", L"<token>"}}));
        VERIFY_ARE_EQUAL(std::wstring{L"wslc-cli-<user>"}, SanitizeUserName(L"wslc-cli-alice", L"alice"));
        VERIFY_ARE_EQUAL(std::wstring{L"<user>-work-<user>"}, SanitizeUserName(L"Alice-work-ALICE", L"alice"));
        VERIFY_ARE_EQUAL(std::wstring{L"custom-session"}, SanitizeUserName(L"custom-session", L"alice"));
        VERIFY_ARE_EQUAL(std::wstring{L"wslc-cli-alice"}, SanitizeUserName(L"wslc-cli-alice", L""));
    }

    TEST_METHOD(DiagnosticMacro_EvaluatesMessageOnlyWhenEnabled)
    {
        TestDiagnosticCallback callback;
        size_t evaluationCount = 0;

        wsl::windows::wslc::diagnostics::DiagnosticReporter disabledDiagnostics{&callback};
        WSLC_DIAG(disabledDiagnostics, WSLCDiagnosticLevelDebug, "disabled-debug", L"value={}", ++evaluationCount);
        VERIFY_ARE_EQUAL(size_t{0}, evaluationCount);
        VERIFY_ARE_EQUAL(size_t{0}, callback.DiagnosticCount);
        VERIFY_ARE_EQUAL(size_t{1}, callback.EnabledLevelsQueryCount);

        callback.EnabledLevels = WSLCDiagnosticLevelDebug;
        WSLC_DIAG(disabledDiagnostics, WSLCDiagnosticLevelDebug, "still-disabled", L"value={}", ++evaluationCount);
        VERIFY_ARE_EQUAL(size_t{0}, evaluationCount);
        VERIFY_ARE_EQUAL(size_t{1}, callback.EnabledLevelsQueryCount);

        wsl::windows::wslc::diagnostics::DiagnosticReporter enabledDiagnostics{&callback};
        WSLC_DIAG(enabledDiagnostics, WSLCDiagnosticLevelDebug, "enabled-debug", "value={}", ++evaluationCount);
        VERIFY_ARE_EQUAL(size_t{1}, evaluationCount);
        VERIFY_ARE_EQUAL(size_t{1}, callback.DiagnosticCount);
        VERIFY_ARE_EQUAL(size_t{2}, callback.EnabledLevelsQueryCount);
        VERIFY_ARE_EQUAL(std::string{"enabled-debug"}, callback.LastCode);
        VERIFY_ARE_EQUAL(std::wstring{L"value=1"}, callback.LastMessage);
    }

    TEST_METHOD(DiagnosticEvent_EvaluatesMessageOnceForEnabledSinks)
    {
        TestDiagnosticCallback callback;
        size_t evaluationCount = 0;
        size_t traceCount = 0;

        wsl::windows::wslc::diagnostics::DiagnosticReporter disabledDiagnostics{&callback};
        wsl::windows::wslc::events::Dispatch(
            false,
            false,
            [&]() {
                ++evaluationCount;
                return std::wstring{L"disabled"};
            },
            [&](const std::wstring&) { ++traceCount; },
            [&](const std::wstring& message) {
                disabledDiagnostics.Report(WSLCDiagnosticLevelDebug, "disabled-event", message.c_str());
            });
        VERIFY_ARE_EQUAL(size_t{0}, evaluationCount);
        VERIFY_ARE_EQUAL(size_t{0}, traceCount);
        VERIFY_ARE_EQUAL(size_t{0}, callback.DiagnosticCount);

        wsl::windows::wslc::events::Dispatch(
            true,
            false,
            [&]() {
                ++evaluationCount;
                return std::wstring{L"trace-only"};
            },
            [&](const std::wstring& message) {
                ++traceCount;
                VERIFY_ARE_EQUAL(std::wstring{L"trace-only"}, message);
            },
            [&](const std::wstring& message) {
                disabledDiagnostics.Report(WSLCDiagnosticLevelDebug, "trace-only-event", message.c_str());
            });
        VERIFY_ARE_EQUAL(size_t{1}, evaluationCount);
        VERIFY_ARE_EQUAL(size_t{1}, traceCount);
        VERIFY_ARE_EQUAL(size_t{0}, callback.DiagnosticCount);

        callback.EnabledLevels = WSLCDiagnosticLevelDebug;
        wsl::windows::wslc::diagnostics::DiagnosticReporter enabledDiagnostics{&callback};
        wsl::windows::wslc::events::Dispatch(
            true,
            true,
            [&]() {
                ++evaluationCount;
                return std::wstring{L"enabled"};
            },
            [&](const std::wstring& message) {
                ++traceCount;
                VERIFY_ARE_EQUAL(std::wstring{L"enabled"}, message);
            },
            [&](const std::wstring& message) {
                enabledDiagnostics.Report(WSLCDiagnosticLevelDebug, "enabled-event", message.c_str());
            });

        VERIFY_ARE_EQUAL(size_t{2}, evaluationCount);
        VERIFY_ARE_EQUAL(size_t{2}, traceCount);
        VERIFY_ARE_EQUAL(size_t{1}, callback.DiagnosticCount);
        VERIFY_ARE_EQUAL(std::string{"enabled-event"}, callback.LastCode);
        VERIFY_ARE_EQUAL(std::wstring{L"enabled"}, callback.LastMessage);
    }

    TEST_METHOD(Terminal_SetNoColorTogglesIsNoColor)
    {
        CaptureTerminal cap;
        VERIFY_IS_FALSE(cap.terminal.IsNoColor());
        cap.terminal.SetNoColor(true);
        VERIFY_IS_TRUE(cap.terminal.IsNoColor());
        cap.terminal.SetNoColor(false);
        VERIFY_IS_FALSE(cap.terminal.IsNoColor());
    }

    TEST_METHOD(Terminal_IsVTEnabledReflectsPerChannelState)
    {
        {
            SplitCaptureTerminal cap{/*vt*/ false};
            VERIFY_IS_FALSE(cap.terminal.IsVTEnabled(Terminal::Level::Output));
            VERIFY_IS_FALSE(cap.terminal.IsVTEnabled(Terminal::Level::Error));
        }
        {
            SplitCaptureTerminal cap{/*vt*/ true};
            VERIFY_IS_TRUE(cap.terminal.IsVTEnabled(Terminal::Level::Output));
            VERIFY_IS_TRUE(cap.terminal.IsVTEnabled(Terminal::Level::Error));
            VERIFY_IS_TRUE(cap.terminal.IsVTEnabled(Terminal::Level::Debug));
        }
        {
            CapturePipe outPipe;
            CapturePipe errPipe;
            Terminal terminal{outPipe.file(), /*outVt*/ true, errPipe.file(), /*errVt*/ false};
            VERIFY_IS_TRUE(terminal.IsVTEnabled(Terminal::Level::Output));
            VERIFY_IS_FALSE(terminal.IsVTEnabled(Terminal::Level::Info));
            VERIFY_IS_FALSE(terminal.IsVTEnabled(Terminal::Level::Debug));
            VERIFY_IS_FALSE(terminal.IsVTEnabled(Terminal::Level::Warning));
            VERIFY_IS_FALSE(terminal.IsVTEnabled(Terminal::Level::Error));
        }
    }

    TEST_METHOD(Terminal_IsColorEnabledPerLevelHonorsBothVTAndNoColor)
    {
        SplitCaptureTerminal cap{/*vt*/ true};
        VERIFY_IS_TRUE(cap.terminal.IsColorEnabled(Terminal::Level::Output));
        VERIFY_IS_TRUE(cap.terminal.IsColorEnabled(Terminal::Level::Error));
        VERIFY_IS_TRUE(cap.terminal.IsColorEnabled(Terminal::Level::Debug));

        cap.terminal.SetNoColor(true);
        VERIFY_IS_FALSE(cap.terminal.IsColorEnabled(Terminal::Level::Output));
        VERIFY_IS_FALSE(cap.terminal.IsColorEnabled(Terminal::Level::Error));
        VERIFY_IS_FALSE(cap.terminal.IsColorEnabled(Terminal::Level::Debug));
    }

    TEST_METHOD(Terminal_GetConsoleWidthReturnsNulloptForFileChannels)
    {
        SplitCaptureTerminal cap;
        VERIFY_IS_FALSE(cap.terminal.GetConsoleWidth(Terminal::Level::Output).has_value());
        VERIFY_IS_FALSE(cap.terminal.GetConsoleWidth(Terminal::Level::Info).has_value());
        VERIFY_IS_FALSE(cap.terminal.GetConsoleWidth(Terminal::Level::Debug).has_value());
        VERIFY_IS_FALSE(cap.terminal.GetConsoleWidth(Terminal::Level::Warning).has_value());
        VERIFY_IS_FALSE(cap.terminal.GetConsoleWidth(Terminal::Level::Error).has_value());
    }

    TEST_METHOD(Terminal_Write_MixesSequencesWithStandardFormatArgs)
    {
        // Terminal.Write is std::format under the hood — any formattable type works
        // alongside Sequences. Sequences are stripped when color is off; everything
        // else formats normally through std::format machinery.
        //
        // This test exercises four sequence categories in a single format call:
        //   SGR color   (Format::Fg::BrightRed)  — color, stripped by NoColor
        //   Non-color   (Erase::LineForward)      — not color, survives NoColor
        //   Hyperlink   (ConstructedSequence OSC8) — color, stripped by NoColor
        //   SGR reset   (Format::Default)         — color, stripped by NoColor
        //
        // Hyperlink open/close are separate sequences so the visible link text
        // degrades gracefully when sequences are stripped.

        const auto& eraseLine = Erase::LineForward; // \x1b[K  — non-color CSI
        const auto linkOpen = Format::LinkOpen(L"https://example.com");
        const auto& linkClose = Format::LinkClose;

        // Format: <color>Count: <int>, hex: <hex>, <erase><linkOpen>click here<linkClose><reset>
        constexpr auto fmt = L"{}Count: {}, hex: {:04x}, {}{}click here{}{}\n";

        // VT + color enabled: equivalent to std::format with all sequence bytes.
        {
            CaptureTerminal cap{/*vtEnabled*/ true};
            cap.terminal.Output(fmt, Format::Fg::BrightRed, 42, 255u, eraseLine, linkOpen, linkClose, Format::Default);

            const auto expected = std::format(
                fmt, Format::Fg::BrightRed.Get(), 42, 255u, eraseLine.Get(), linkOpen.Get(), linkClose.Get(), Format::Default.Get());
            VERIFY_ARE_EQUAL(expected, cap.captured());
        }

        // NoColor (VT enabled, color disabled): non-color sequences pass through,
        // color sequences (SGR, hyperlink) replaced with empty string.
        {
            CaptureTerminal cap{/*vtEnabled*/ true};
            cap.terminal.SetNoColor(true);
            cap.terminal.Output(fmt, Format::Fg::BrightRed, 42, 255u, eraseLine, linkOpen, linkClose, Format::Default);

            const std::wstring_view empty;
            const auto expected = std::format(fmt, empty, 42, 255u, eraseLine.Get(), empty, empty, empty);
            VERIFY_ARE_EQUAL(expected, cap.captured());
        }

        // VT disabled: all sequences replaced with empty string.
        {
            CaptureTerminal cap{/*vtEnabled*/ false};
            cap.terminal.Output(fmt, Format::Fg::BrightRed, 42, 255u, eraseLine, linkOpen, linkClose, Format::Default);

            const std::wstring_view empty;
            const auto expected = std::format(fmt, empty, 42, 255u, empty, empty, empty, empty);
            VERIFY_ARE_EQUAL(expected, cap.captured());
        }
    }

    TEST_METHOD(InputChannel_ReadLineReturnsNulloptAtEof)
    {
        InputPipe pipe{L""};
        const InputChannel channel{pipe.file(), false};
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(InputChannel_ReadLineSplitsOnNewline)
    {
        InputPipe pipe{L"user\npass\n"};
        const InputChannel channel{pipe.file(), false};

        auto first = channel.ReadLine(false);
        VERIFY_IS_TRUE(first.has_value());
        VERIFY_ARE_EQUAL(std::wstring{L"user"}, first.value());

        auto second = channel.ReadLine(false);
        VERIFY_IS_TRUE(second.has_value());
        VERIFY_ARE_EQUAL(std::wstring{L"pass"}, second.value());

        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(InputChannel_ReadLineStripsCarriageReturn)
    {
        InputPipe pipe{L"user\r\npass\r\n"};
        const InputChannel channel{pipe.file(), false};

        VERIFY_ARE_EQUAL(std::wstring{L"user"}, channel.ReadLine(false).value_or(L"<eof>"));
        VERIFY_ARE_EQUAL(std::wstring{L"pass"}, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineReturnsEmptyStringForBlankLine)
    {
        // A bare empty line is distinct from EOF: value present but empty.
        InputPipe pipe{L"\nsecond\n"};
        const InputChannel channel{pipe.file(), false};

        auto blank = channel.ReadLine(false);
        VERIFY_IS_TRUE(blank.has_value());
        VERIFY_ARE_EQUAL(std::wstring{L""}, blank.value());

        VERIFY_ARE_EQUAL(std::wstring{L"second"}, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineReturnsFinalLineWithoutTrailingNewline)
    {
        InputPipe pipe{L"only"};
        const InputChannel channel{pipe.file(), false};

        VERIFY_ARE_EQUAL(std::wstring{L"only"}, channel.ReadLine(false).value_or(L"<eof>"));
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(InputChannel_IsInteractiveReflectsOverrideForNonConsole)
    {
        InputPipe pipe{L"x\n"};
        const InputChannel notInteractive{pipe.file(), false};
        VERIFY_IS_FALSE(notInteractive.IsInteractive());

        InputPipe pipe2{L"x\n"};
        const InputChannel interactive{pipe2.file(), true};
        VERIFY_IS_TRUE(interactive.IsInteractive());
    }

    TEST_METHOD(InputChannel_ReadLineWithMaskReadsWhenNoConsole)
    {
        // Masking is a no-op without a real console; the read still succeeds.
        InputPipe pipe{L"secret\n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(std::wstring{L"secret"}, channel.ReadLine(true).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineOnNullFileReturnsNullopt)
    {
        const InputChannel channel{static_cast<FILE*>(nullptr), false};
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(InputChannel_ReadLinePreservesInteriorAndSurroundingWhitespace)
    {
        // Only the trailing CR/LF is stripped. Leading, interior, and trailing spaces
        // and tabs are preserved verbatim (WSLC does not trim, unlike Docker's prompt).
        InputPipe pipe{L"  spaced \t value  \n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(std::wstring{L"  spaced \t value  "}, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineStripsLoneTrailingCarriageReturnAtEof)
    {
        // A lone trailing CR (no following LF) is not collapsed by the stream's CRLF
        // translation, so it reaches ReadLine and exercises the trailing-CR strip.
        InputPipe pipe{L"value\r"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(std::wstring{L"value"}, channel.ReadLine(false).value_or(L"<eof>"));
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(InputChannel_ReadLinePreservesEmbeddedCarriageReturn)
    {
        // Only a trailing CR is stripped; a CR in the middle of a line is preserved.
        InputPipe pipe{L"a\rb\n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(std::wstring{L"a\rb"}, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineDecodesUnicode)
    {
        // UTF-8 bytes on the wire decode back to the original wide characters.
        const std::wstring expected = L"\u00e9\u4e2d\u6587\u2013user";
        InputPipe pipe{expected + L"\n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(expected, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineHandlesLongLine)
    {
        // Lines longer than any internal buffer are read in full (fgetwc loop).
        const std::wstring expected(8192, L'z');
        InputPipe pipe{expected + L"\n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(expected, channel.ReadLine(false).value_or(L"<eof>"));
    }

    TEST_METHOD(InputChannel_ReadLineReturnsNulloptAfterAllLinesConsumed)
    {
        InputPipe pipe{L"one\ntwo\n"};
        const InputChannel channel{pipe.file(), false};
        VERIFY_ARE_EQUAL(std::wstring{L"one"}, channel.ReadLine(false).value_or(L"<eof>"));
        VERIFY_ARE_EQUAL(std::wstring{L"two"}, channel.ReadLine(false).value_or(L"<eof>"));
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
        // Further reads keep returning nullopt (idempotent at EOF).
        VERIFY_IS_FALSE(channel.ReadLine(false).has_value());
    }

    TEST_METHOD(Terminal_ReadLineReturnsInput)
    {
        InputCaptureTerminal cap{L"line1\nline2\n"};
        VERIFY_ARE_EQUAL(std::wstring{L"line1"}, cap.terminal.ReadLine().value_or(L"<eof>"));
        VERIFY_ARE_EQUAL(std::wstring{L"line2"}, cap.terminal.ReadLine().value_or(L"<eof>"));
        VERIFY_IS_FALSE(cap.terminal.ReadLine().has_value());
    }

    TEST_METHOD(Terminal_IsInputInteractiveReflectsChannel)
    {
        InputCaptureTerminal pipeInput{L"x\n", /*interactive*/ false};
        VERIFY_IS_FALSE(pipeInput.terminal.IsInputInteractive());

        InputCaptureTerminal consoleInput{L"x\n", /*interactive*/ true};
        VERIFY_IS_TRUE(consoleInput.terminal.IsInputInteractive());
    }

    TEST_METHOD(Terminal_PromptForLineWritesLabelToStdoutAndReturnsInput)
    {
        InputCaptureTerminal cap{L"myuser\n"};

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Username: ", false);
        VERIFY_ARE_EQUAL(std::wstring{L"myuser"}, result);

        // Label lands on stdout (Docker convention); nothing on stderr; no trailing
        // newline because the input was not masked.
        VERIFY_ARE_EQUAL(std::wstring{L"Username: "}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.errPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineMaskedInteractiveEmitsTrailingNewline)
    {
        // Interactive override makes willMask true, so the un-echoed Enter is advanced
        // with a trailing newline after the label.
        InputCaptureTerminal cap{L"secret\n", /*interactive*/ true};

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Password: ", true);
        VERIFY_ARE_EQUAL(std::wstring{L"secret"}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Password: \n"}, cap.outPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineMaskedNonInteractiveEmitsNoTrailingNewline)
    {
        // Redirected input is not interactive, so no masking and no trailing newline.
        InputCaptureTerminal cap{L"secret\n", /*interactive*/ false};

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Password: ", true);
        VERIFY_ARE_EQUAL(std::wstring{L"secret"}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Password: "}, cap.outPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineReturnsEmptyStringAtEof)
    {
        InputCaptureTerminal cap{L""};
        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Username: ", false);
        VERIFY_ARE_EQUAL(std::wstring{L""}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Username: "}, cap.outPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineEmitsLabelVerbatimWithFormatCharacters)
    {
        // The label is passed as a formatting argument, not a format string, so brace
        // and percent characters in it must never be interpreted (no format injection).
        InputCaptureTerminal cap{L"answer\n"};
        const std::wstring label = L"Value {} {0} {name} 100% ${var}: ";

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, label, false);
        VERIFY_ARE_EQUAL(std::wstring{L"answer"}, result);
        VERIFY_ARE_EQUAL(label, cap.outPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineDoesNotTrimPasswordWhitespace)
    {
        // Secrets are opaque: interior and surrounding whitespace is preserved so a
        // password like "  a b  " is returned exactly as typed.
        InputCaptureTerminal cap{L"  a b  \n", /*interactive*/ true};

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Password: ", true);
        VERIFY_ARE_EQUAL(std::wstring{L"  a b  "}, result);
        VERIFY_ARE_EQUAL(std::wstring{L"Password: \n"}, cap.outPipe.captured());
    }

    TEST_METHOD(Terminal_PromptForLineReturnsUnicodeInput)
    {
        const std::wstring expected = L"\u00fcser\u00f1ame";
        InputCaptureTerminal cap{expected + L"\n"};

        const auto result = cap.terminal.PromptForLine(Terminal::Level::Output, L"Username: ", false);
        VERIFY_ARE_EQUAL(expected, result);
    }

    TEST_METHOD(Terminal_Confirm)
    {
        // The prompt is written inline on stdout with the standard suffix appended, and nothing
        // goes to stderr.
        {
            InputCaptureTerminal cap{L"y\n"};
            VERIFY_IS_TRUE(cap.terminal.Confirm(L"Remove everything?"));
            VERIFY_ARE_EQUAL(std::wstring{L"Remove everything? [y/N] "}, cap.outPipe.captured());
            VERIFY_ARE_EQUAL(std::wstring{L""}, cap.errPipe.captured());
        }

        // Only a bare y accepts, in either case and with surrounding whitespace trimmed.
        for (const auto* answer : {L"y\n", L"Y\n", L"  y  \n"})
        {
            InputCaptureTerminal cap{answer};
            VERIFY_IS_TRUE(cap.terminal.Confirm(L"Remove everything?"));
        }

        // Anything else declines, including a spelled-out yes, matching the container CLI ecosystem.
        // A prune with no input attached must abort rather than block, so end of input declines too.
        for (const auto* answer : {L"yes\n", L"n\n", L"N\n", L"no\n", L"\n", L"maybe\n", L""})
        {
            InputCaptureTerminal cap{answer};
            VERIFY_IS_FALSE(cap.terminal.Confirm(L"Remove everything?"));
            VERIFY_ARE_EQUAL(std::wstring{L"Remove everything? [y/N] "}, cap.outPipe.captured());
        }

        // The message is a formatting argument, not a format string, so braces must not be
        // interpreted.
        {
            InputCaptureTerminal cap{L"y\n"};
            const std::wstring message = L"Remove {} {0} {name} 100%?";

            VERIFY_IS_TRUE(cap.terminal.Confirm(message));
            VERIFY_ARE_EQUAL(message + L" [y/N] ", cap.outPipe.captured());
        }
    }

    TEST_METHOD(Terminal_ReadLineMaskDefaultsToUnmasked)
    {
        // ReadLine(bool mask = false): the default reads without masking and returns
        // the line, used by the --password-stdin path.
        InputCaptureTerminal cap{L"piped-secret\n"};
        VERIFY_ARE_EQUAL(std::wstring{L"piped-secret"}, cap.terminal.ReadLine().value_or(L"<eof>"));
        // Nothing is written for a bare ReadLine (no prompt label).
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.outPipe.captured());
        VERIFY_ARE_EQUAL(std::wstring{L""}, cap.errPipe.captured());
    }
};

} // namespace WSLCCLITerminalUnitTests
