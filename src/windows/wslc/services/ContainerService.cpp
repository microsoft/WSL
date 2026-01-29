#include "ContainerService.h"
#include "../Utils.h"
#include <wslutil.h>
#include <WSLAProcessLauncher.h>

namespace wslc::services
{
using wsl::windows::common::wslutil::WSLAErrorDetails;
using wsl::windows::common::wslutil::PrintMessage;
using wsl::windows::common::ClientRunningWSLAProcess;

int ContainerService::Run(IWSLASession& session, std::string image, RunOptions runOptions)
{
    WSLA_CONTAINER_OPTIONS options{};
    options.Image = image.c_str();

    std::vector<WSLA_PROCESS_FD> fds;
    HANDLE Stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE Stdin = GetStdHandle(STD_INPUT_HANDLE);

    if (runOptions.TTY)
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
        if (runOptions.Interactive)
        {
            fds.emplace_back(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeDefault});
        }

        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeDefault});
        fds.emplace_back(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeDefault});
    }

    std::vector<const char*> args;
    for (const auto& arg : runOptions.Arguments)
    {
        args.push_back(arg.c_str());
    }

    options.InitProcessOptions.CommandLine = args.data();
    options.InitProcessOptions.CommandLineCount = static_cast<ULONG>(args.size());
    options.InitProcessOptions.Fds = fds.data();
    options.InitProcessOptions.FdsCount = static_cast<ULONG>(fds.size());

    wil::com_ptr<IWSLAContainer> container;
    WSLAErrorDetails error{};
    auto result = session.CreateContainer(&options, &container, &error.Error);
    if (result == WSLA_E_IMAGE_NOT_FOUND)
    {
        PrintMessage(std::format(L"Image '{}' not found, pulling", image), stderr);

        PullImpl(session, image);

        error.Reset();
        result = session.CreateContainer(&options, &container, &error.Error);
    }

    error.ThrowIfFailed(result);

    THROW_IF_FAILED(container->Start()); // TODO: Error message

    wil::com_ptr<IWSLAProcess> process;
    THROW_IF_FAILED(container->GetInitProcess(&process));

    return InteractiveShell(ClientRunningWSLAProcess(std::move(process), std::move(fds)), runOptions.TTY);
}

void ContainerService::Create()
{
}

void ContainerService::Start()
{
}

void ContainerService::Stop()
{
}

void ContainerService::Kill()
{
}

void ContainerService::Delete()
{
}

void ContainerService::List()
{
}

void ContainerService::Exec()
{
}

void ContainerService::Inspect()
{
}
}

