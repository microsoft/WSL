/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImagePruneCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "ImageCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Image Prune Command
std::vector<Argument> ImagePruneCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::All, L"Remove all unused images not referenced by any container"),
        Argument::Create(ArgType::SessionId),
    };
}

std::wstring_view ImagePruneCommand::ShortDescription() const
{
    return {L"Removes unused images."};
}

std::wstring_view ImagePruneCommand::LongDescription() const
{
    return {
        L"Removes dangling (untagged) images in order to reclaim disk space. Using ï¿½a option removes all unused images that "
        L"are "
        L"not referenced by any container."};
}

void ImagePruneCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Image Prune subcommand executing..");
}
} // namespace wsl::windows::wslc
