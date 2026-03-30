/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InspectCommand.cpp

Abstract:

    Implementation of the root-level inspect command, which supports --type
    to inspect containers or images (matching Docker's `docker inspect --type` behavior).
    Defaults to container inspection when --type is not specified.

--*/
#include "CLIExecutionContext.h"
#include "ContainerTasks.h"
#include "ImageTasks.h"
#include "InspectCommand.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared::string;

namespace wsl::windows::wslc {

std::vector<Argument> InspectCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true, NO_LIMIT, L"ID of the resource to inspect"),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::Type),
    };
}

std::wstring InspectCommand::ShortDescription() const
{
    return {L"Return low-level information on a resource."};
}

std::wstring InspectCommand::LongDescription() const
{
    return {L"Display detailed information on one or more resources. Use --type to specify the resource type (container or image). "
            L"Defaults to container if not specified."};
}

void InspectCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    std::wstring type = L"container";
    if (context.Args.Contains(ArgType::Type))
    {
        type = context.Args.Get<ArgType::Type>();
    }

    if (IsEqual(type, L"image"))
    {
        // Reinterpret the ContainerId positional args as ImageId for image inspection.
        auto ids = context.Args.GetAll<ArgType::ContainerId>();
        for (const auto& id : ids)
        {
            context.Args.Add<ArgType::ImageId>(std::wstring(id));
        }

        context << CreateSession << InspectImages;
    }
    else
    {
        context << CreateSession << InspectContainers;
    }
}
} // namespace wsl::windows::wslc
