#include "Utils.h"
#include "wslutil.h"
#include "wslaservice.h"
#include "WslSecurity.h"
#include "WSLAProcessLauncher.h"
#include "ExecutionContext.h"
#include <thread>
#include <format>
#include "ImageService.h"

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::WSLAProcessLauncher;
using wsl::windows::common::relay::EventHandle;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::common::relay::RelayHandle;
using wsl::windows::common::wslutil::WSLAErrorDetails;

DEFINE_ENUM_FLAG_OPERATORS(WSLASessionFlags);

wil::com_ptr<IWSLASession> OpenCLISession()
{
    wil::com_ptr<IWSLASessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    auto dataFolder = std::filesystem::path(wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr)) / "wsla";

    // TODO: Have a configuration file for those.
    WSLA_SESSION_SETTINGS settings{};
    settings.DisplayName = L"wsla-cli";
    settings.CpuCount = 4;
    settings.MemoryMb = 2048;
    settings.BootTimeoutMs = 30 * 1000;
    settings.StoragePath = dataFolder.c_str();
    settings.MaximumStorageSizeMb = 10000; // 10GB.
    settings.NetworkingMode = WSLANetworkingModeNAT;

    wil::com_ptr<IWSLASession> session;
    auto x = sessionManager->CreateSession(&settings, WSLASessionFlagsPersistent | WSLASessionFlagsOpenExisting, &session);
    THROW_IF_FAILED(x);
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

    return session;
}

int InteractiveShell(ClientRunningWSLAProcess&& Process, bool Tty)
{
    HANDLE Stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE Stdin = GetStdHandle(STD_INPUT_HANDLE);
    auto exitEvent = Process.GetExitEvent();

    if (Tty)
    {
        // Save original console modes so they can be restored on exit.
        DWORD OriginalInputMode{};
        DWORD OriginalOutputMode{};
        UINT OriginalOutputCP = GetConsoleOutputCP();
        THROW_LAST_ERROR_IF(!::GetConsoleMode(Stdin, &OriginalInputMode));
        THROW_LAST_ERROR_IF(!::GetConsoleMode(Stdout, &OriginalOutputMode));

        auto restoreConsoleMode = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            SetConsoleMode(Stdin, OriginalInputMode);
            SetConsoleMode(Stdout, OriginalOutputMode);
            SetConsoleOutputCP(OriginalOutputCP);
        });

        // Configure console for interactive usage.
        DWORD InputMode = OriginalInputMode;
        WI_SetAllFlags(InputMode, (ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT));
        WI_ClearAllFlags(InputMode, (ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT));
        THROW_IF_WIN32_BOOL_FALSE(::SetConsoleMode(Stdin, InputMode));

        DWORD OutputMode = OriginalOutputMode;
        WI_SetAllFlags(OutputMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
        THROW_IF_WIN32_BOOL_FALSE(::SetConsoleMode(Stdout, OutputMode));

        THROW_LAST_ERROR_IF(!::SetConsoleOutputCP(CP_UTF8));

        auto processTty = Process.GetStdHandle(WSLAFDTty);

        // TODO: Study a single thread for both handles.

        // Create a thread to relay stdin to the pipe.
        std::thread inputThread([&]() {
            auto updateTerminal = [&Stdout, &Process, &processTty]() {
                CONSOLE_SCREEN_BUFFER_INFOEX info{};
                info.cbSize = sizeof(info);

                THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfoEx(Stdout, &info));

                LOG_IF_FAILED(Process.Get().ResizeTty(
                    info.srWindow.Bottom - info.srWindow.Top + 1, info.srWindow.Right - info.srWindow.Left + 1));
            };

            wsl::windows::common::relay::StandardInputRelay(Stdin, processTty.get(), updateTerminal, exitEvent.get());
        });

        auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            exitEvent.SetEvent();
            inputThread.join();
        });

        // Relay the contents of the pipe to stdout.
        wsl::windows::common::relay::InterruptableRelay(processTty.get(), Stdout);

        // Wait for the process to exit.
        THROW_LAST_ERROR_IF(WaitForSingleObject(exitEvent.get(), INFINITE) != WAIT_OBJECT_0);
    }
    else
    {
        wsl::windows::common::relay::MultiHandleWait io;

        // Create a thread to relay stdin to the pipe.

        std::thread inputThread;

        auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (inputThread.joinable())
            {
                exitEvent.SetEvent();
                inputThread.join();
            }
        });

        // Required because ReadFile() blocks if stdin is a tty.
        if (wsl::windows::common::wslutil::IsInteractiveConsole())
        {
            // TODO: Will output CR instead of LF's which can confuse the linux app.
            // Consider a custom relay logic to fix this.
            inputThread = std::thread{
                [&]() { wsl::windows::common::relay::InterruptableRelay(Stdin, Process.GetStdHandle(0).get(), exitEvent.get()); }};
        }
        else
        {
            io.AddHandle(std::make_unique<RelayHandle>(GetStdHandle(STD_INPUT_HANDLE), Process.GetStdHandle(0)));
        }

        io.AddHandle(std::make_unique<RelayHandle>(Process.GetStdHandle(1), GetStdHandle(STD_OUTPUT_HANDLE)));
        io.AddHandle(std::make_unique<RelayHandle>(Process.GetStdHandle(2), GetStdHandle(STD_ERROR_HANDLE)));
        io.AddHandle(std::make_unique<EventHandle>(exitEvent.get()));

        io.Run({});
    }

    int exitCode = Process.GetExitCode();

    return exitCode;
}

void PullImpl(IWSLASession& Session, const std::string& Image)
{
    HANDLE Stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    // Configure console for interactive usage.
    DWORD OriginalOutputMode{};
    UINT OriginalOutputCP = GetConsoleOutputCP();
    THROW_LAST_ERROR_IF(!::GetConsoleMode(Stdout, &OriginalOutputMode));

    DWORD OutputMode = OriginalOutputMode;
    WI_SetAllFlags(OutputMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    THROW_IF_WIN32_BOOL_FALSE(::SetConsoleMode(Stdout, OutputMode));

    THROW_LAST_ERROR_IF(!::SetConsoleOutputCP(CP_UTF8));

    // TODO: Handle terminal resizes.
    class DECLSPEC_UUID("7A1D3376-835A-471A-8DC9-23653D9962D0") Callback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
    {
    public:
        auto MoveToLine(SHORT Line, bool Revert = true)
        {
            if (Line > 0)
            {
                wprintf(L"\033[%iA", Line);
            }

            return wil::scope_exit([Line = Line]() {
                if (Line > 1)
                {
                    wprintf(L"\033[%iB", Line - 1);
                }
            });
        }

        HRESULT OnProgress(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total) override
        try
        {
            if (Id == nullptr || *Id == '\0') // Print all 'global' statuses on their own line
            {
                wprintf(L"%hs\n", Status);
                m_currentLine++;
                return S_OK;
            }

            auto info = Info();

            auto it = m_statuses.find(Id);
            if (it == m_statuses.end())
            {
                // If this is the first time we see this ID, create a new line for it.
                m_statuses.emplace(Id, m_currentLine);
                wprintf(L"%ls\n", GenerateStatusLine(Status, Id, Current, Total, info).c_str());
                m_currentLine++;
            }
            else
            {
                auto revert = MoveToLine(m_currentLine - it->second);
                wprintf(L"%ls\n", GenerateStatusLine(Status, Id, Current, Total, info).c_str());
            }

            return S_OK;
        }
        CATCH_RETURN();

    private:
        static CONSOLE_SCREEN_BUFFER_INFO Info()
        {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));

            return info;
        }

        std::wstring GenerateStatusLine(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total, const CONSOLE_SCREEN_BUFFER_INFO& Info)
        {
            std::wstring line;
            if (Total != 0)
            {
                line = std::format(L"{} '{}': {}%", Status, Id, Current * 100 / Total);
            }
            else if (Current != 0)
            {
                line = std::format(L"{} '{}': {}s", Status, Id, Current);
            }
            else
            {
                line = std::format(L"{} '{}'", Status, Id);
            }

            // Erase any previously written char on that line.
            while (line.size() < Info.dwSize.X)
            {
                line += L' ';
            }

            return line;
        }

        std::map<std::string, SHORT> m_statuses;
        SHORT m_currentLine = 0;
        ChangeTerminalMode m_terminalMode{GetStdHandle(STD_OUTPUT_HANDLE), false};
    };

    wil::com_ptr<IWSLASession> session = OpenCLISession();

    wslc::services::ImageService imageService;
    Callback callback;
    imageService.Pull(Image, &callback);
}

int ReportError(const std::wstring& context, HRESULT hr)
{
    auto errorString = wsl::windows::common::wslutil::ErrorCodeToString(hr);
    wslutil::PrintMessage(Localization::MessageErrorCode(context, errorString), stderr);
    return 1;
}
