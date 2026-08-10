/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIBuildImageCallbackUnitTests.cpp

Abstract:

    Unit tests for BuildImageCallback progress rendering.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "BuildImageCallback.h"
#include "ContainerModel.h"
#include "Terminal.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIBuildImageCallbackUnitTests {

namespace {

    // Drives a callback through a representative build: a step header, multi-line log output,
    // a \r-based in-place progress update, and a pull progress entry. Returns everything the
    // terminal emitted, captured after the callback is destroyed so its final frame is included.
    std::wstring RunBuild(ProgressMode mode, bool vtEnabled, bool verbose = false)
    {
        CaptureTerminal capture(vtEnabled);
        wil::unique_event cancelEvent;
        cancelEvent.create(wil::EventOptions::ManualReset);

        {
            BuildImageCallback callback(capture.terminal, cancelEvent.get(), verbose, mode);

            VERIFY_SUCCEEDED(callback.OnProgress("#1 [1/2] FROM debian:latest\n", "", 0, 0));
            VERIFY_SUCCEEDED(callback.OnProgress("first log line\nsecond log line\n", "log", 0, 0));
            VERIFY_SUCCEEDED(callback.OnProgress("downloading 10%\rdownloading 80%\r", "log", 0, 0));
            VERIFY_SUCCEEDED(callback.OnProgress("sha256:abc downloading", "sha256:abc", 50, 100));
            VERIFY_SUCCEEDED(callback.OnProgress("#2 [2/2] RUN echo hi\n", "", 0, 0));
        }

        return capture.captured();
    }

} // namespace

class WSLCCLIBuildImageCallbackUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIBuildImageCallbackUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    // plain must never redraw. Even when a VT console is attached (the case an E2E test with
    // redirected output cannot reach), the output must be append-only: no cursor movement, no
    // erases, and no escape sequences of any kind.
    TEST_METHOD(BuildImageCallback_PlainOnVtConsole_EmitsNoEscapeSequences)
    {
        const auto output = RunBuild(ProgressMode::Plain, true);

        VERIFY_IS_TRUE(output.find(L'\x1b') == std::wstring::npos, L"plain mode must not emit any VT escape sequence");
        VERIFY_IS_TRUE(output.find(L"#1 [1/2] FROM debian:latest") != std::wstring::npos, L"build steps must still be reported");
        VERIFY_IS_TRUE(output.find(L"#2 [2/2] RUN echo hi") != std::wstring::npos, L"build steps must still be reported");
    }

    // Control: the same input in tty mode does emit cursor control. Without this, the test above
    // could pass simply because the rendering path was never exercised.
    TEST_METHOD(BuildImageCallback_TtyOnVtConsole_EmitsEscapeSequences)
    {
        const auto output = RunBuild(ProgressMode::Tty, true);

        VERIFY_IS_TRUE(output.find(L'\x1b') != std::wstring::npos, L"tty mode is expected to render in place using VT sequences");
    }

    // plain must produce the same escape-free output regardless of whether a console is attached,
    // so redirecting a plain build to a file yields exactly what was shown on screen.
    TEST_METHOD(BuildImageCallback_PlainMatchesRedirectedOutput)
    {
        const auto onConsole = RunBuild(ProgressMode::Plain, true);
        const auto redirected = RunBuild(ProgressMode::Plain, false);

        VERIFY_ARE_EQUAL(redirected, onConsole);
    }

    // quiet suppresses progress entirely on success, and must not emit cursor control either.
    TEST_METHOD(BuildImageCallback_QuietOnVtConsole_EmitsNothing)
    {
        const auto output = RunBuild(ProgressMode::Quiet, true);

        VERIFY_ARE_EQUAL(std::wstring{L""}, output);
    }

    // rawjson forwards the server's payload verbatim, so it must not be wrapped in cursor control.
    TEST_METHOD(BuildImageCallback_RawJsonOnVtConsole_EmitsNoEscapeSequences)
    {
        const auto output = RunBuild(ProgressMode::RawJson, true);

        VERIFY_IS_TRUE(output.find(L'\x1b') == std::wstring::npos, L"rawjson mode must forward payloads without VT sequences");
    }
};

} // namespace WSLCCLIBuildImageCallbackUnitTests
