/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeService.cpp

Abstract:

    Implements minimal compose CLI operations.

--*/

#include "precomp.h"
#include "ComposeService.h"
#include "ConsoleService.h"
#include <relay.hpp>
#include <WSLCProcessLauncher.h>

namespace wsl::windows::wslc::services {

wil::com_ptr<IWSLCComposeSession> ComposeService::Open(models::Session& Session, const std::wstring& Path)
{
    wil::com_ptr<IWSLCComposeSession> composeSession;
    THROW_IF_FAILED(Session.Get()->CreateComposeSession(Path.c_str(), &composeSession));
    return composeSession;
}

void ComposeService::Create(models::Session& Session, const std::wstring& Path)
{
    Open(Session, Path);
}

int ComposeService::Up(Terminal& Terminal, models::Session& Session, const std::wstring& Path)
{
    Start(Session, Path);
    return Attach(Terminal, Session, Path);
}

void ComposeService::Start(models::Session& Session, const std::wstring& Path)
{
    THROW_IF_FAILED(Open(Session, Path)->Start());
}

int ComposeService::Attach(Terminal& Terminal, models::Session& Session, const std::wstring& Path)
{
    [[maybe_unused]] auto operation = Session.BeginContainerOperation();
    auto composeSession = Open(Session, Path);

    wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
    THROW_IF_FAILED(composeSession->ListContainers(&containers, containers.size_address<ULONG>()));

    common::io::MultiHandleWait io;

    for (const auto& e : containers)
    {
        wil::com_ptr<IWSLCContainer> container;
        THROW_IF_FAILED(Session.Get()->OpenContainer(e.Id, &container));

        wil::com_ptr<IWSLCProcess> process;
        THROW_IF_FAILED(container->GetInitProcess(&process));

        WSLCProcessFlags flags{};
        THROW_IF_FAILED(process->GetFlags(&flags));

        wsl::windows::common::wslutil::COMOutputHandle stdinHandle;
        wsl::windows::common::wslutil::COMOutputHandle stdoutHandle;
        wsl::windows::common::wslutil::COMOutputHandle stderrHandle;

        THROW_IF_FAILED(container->Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle));

        // TODO: Add support for stdin, tty processes, stop on ctrl-c.

        io.AddHandle(std::make_unique<wsl::windows::common::io::RelayHandle<wsl::windows::common::io::ReadHandle>>(
            stdoutHandle.Release(), GetStdHandle(STD_OUTPUT_HANDLE)));

        io.AddHandle(std::make_unique<wsl::windows::common::io::RelayHandle<wsl::windows::common::io::ReadHandle>>(
            stderrHandle.Release(), GetStdHandle(STD_ERROR_HANDLE)));
    }

    io.Run({});

   
    return 0;
}

void ComposeService::Stop(models::Session& Session, const std::wstring& Path, ULONG Timeout)
{
    THROW_IF_FAILED(Open(Session, Path)->Stop(Timeout));
}

} // namespace wsl::windows::wslc::services
