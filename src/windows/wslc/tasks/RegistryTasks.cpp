/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryTasks.cpp

Abstract:

    Implementation of registry command related execution logic.

--*/
#include "Argument.h"
#include "CLIExecutionContext.h"
#include "RegistryService.h"
#include "RegistryTasks.h"
#include "Task.h"

using namespace wsl::shared;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {

void Login(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::Username));
    WI_ASSERT(context.Args.Contains(ArgType::Password));

    auto& session = context.Data.Get<Data::Session>();

    auto username = WideToMultiByte(context.Args.Get<ArgType::Username>());
    auto password = WideToMultiByte(context.Args.Get<ArgType::Password>());

    auto serverAddress = std::string(RegistryService::DefaultServer);

    if (context.Args.Contains(ArgType::Server))
    {
        serverAddress = WideToMultiByte(context.Args.Get<ArgType::Server>());
    }

    auto [credUsername, credSecret] = RegistryService::Authenticate(session, serverAddress, username, password);
    RegistryService::Store(serverAddress, credUsername, credSecret);

    PrintMessage(Localization::WSLCCLI_LoginSucceeded());
}

void Logout(CLIExecutionContext& context)
{
    auto serverAddress = std::string(RegistryService::DefaultServer);

    if (context.Args.Contains(ArgType::Server))
    {
        serverAddress = WideToMultiByte(context.Args.Get<ArgType::Server>());
    }

    RegistryService::Erase(serverAddress);

    PrintMessage(Localization::WSLCCLI_LogoutSucceeded(MultiByteToWide(serverAddress)));
}

} // namespace wsl::windows::wslc::task
