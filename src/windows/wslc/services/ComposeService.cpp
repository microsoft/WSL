/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeService.cpp

Abstract:

    Implements minimal compose CLI operations.

--*/

#include "precomp.h"
#include "ComposeService.h"
#include "ContainerService.h"

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

void ComposeService::Start(models::Session& Session, const std::wstring& Path)
{
    THROW_IF_FAILED(Open(Session, Path)->Start());
}

int ComposeService::Attach(Terminal& Terminal, models::Session& Session, const std::wstring& Path)
{
    auto composeSession = Open(Session, Path);
    THROW_IF_FAILED(composeSession->Attach());

    wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
    THROW_IF_FAILED(composeSession->ListContainers(&containers, containers.size_address<ULONG>()));
    THROW_HR_WITH_USER_ERROR_IF(
        E_NOTIMPL, wsl::shared::Localization::MessageWslcComposeAttachRequiresSingleContainer(), containers.size() != 1);

    return ContainerService::Attach(Terminal, Session, containers[0].Id);
}

void ComposeService::Stop(models::Session& Session, const std::wstring& Path, ULONG Timeout)
{
    THROW_IF_FAILED(Open(Session, Path)->Stop(Timeout));
}

} // namespace wsl::windows::wslc::services
