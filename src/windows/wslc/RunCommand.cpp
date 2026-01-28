#include "precomp.h"
#include "RunCommand.h"
#include "Utils.h"
#include "WSLAProcessLauncher.h"
#include <CommandLine.h>

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

int RunCommand(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 2, true);

    bool interactive{};
    bool tty{};
    std::string image;
    parser.AddPositionalArgument(Utf8String{image}, 0);
    parser.AddArgument(interactive, L"--interactive", 'i');
    parser.AddArgument(tty, L"--tty", 't');

    parser.Parse();
    THROW_HR_IF(E_INVALIDARG, image.empty());

    auto session = OpenCLISession();

    WSLA_CONTAINER_OPTIONS options{};
    options.Image = image.c_str();

    std::vector<WSLA_PROCESS_FD> fds;
    HANDLE Stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE Stdin = GetStdHandle(STD_INPUT_HANDLE);

    if (tty)
    {
        CONSOLE_SCREEN_BUFFER_INFOEX Info{};
        Info.cbSize = sizeof(Info);
        THROW_IF_WIN32_BOOL_FALSE(::GetConsoleScreenBufferInfoEx(Stdout, &Info));

        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeTerminalInput});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeTerminalOutput});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeTerminalControl});

        options.InitProcessOptions.TtyColumns = Info.srWindow.Right - Info.srWindow.Left + 1;
        options.InitProcessOptions.TtyRows = Info.srWindow.Bottom - Info.srWindow.Top + 1;
    }
    else
    {
        if (interactive)
        {
            fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeDefault});
        }

        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeDefault});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeDefault});
    }

    std::vector<std::string> argsStorage;
    std::vector<const char*> args;
    for (size_t i = parser.ParseIndex(); i < parser.Argc(); i++)
    {
        argsStorage.emplace_back(wsl::shared::string::WideToMultiByte(parser.Argv(i)));
    }

    for (const auto& e : argsStorage)
    {
        args.emplace_back(e.c_str());
    }

    options.InitProcessOptions.CommandLine = args.data();
    options.InitProcessOptions.CommandLineCount = static_cast<ULONG>(args.size());
    options.InitProcessOptions.Fds = fds.data();
    options.InitProcessOptions.FdsCount = static_cast<ULONG>(fds.size());

    wil::com_ptr<IWSLAContainer> container;
    WSLAErrorDetails error{};
    auto result = session->CreateContainer(&options, &container, &error.Error);
    if (result == WSLA_E_IMAGE_NOT_FOUND)
    {
        wslutil::PrintMessage(std::format(L"Image '{}' not found, pulling", image), stderr);

        PullImpl(*session.get(), image);

        error.Reset();
        result = session->CreateContainer(&options, &container, &error.Error);
    }

    error.ThrowIfFailed(result);

    THROW_IF_FAILED(container->Start()); // TODO: Error message

    wil::com_ptr<IWSLAProcess> process;
    THROW_IF_FAILED(container->GetInitProcess(&process));

    return InteractiveShell(ClientRunningWSLAProcess(std::move(process), std::move(fds)), tty);
}
