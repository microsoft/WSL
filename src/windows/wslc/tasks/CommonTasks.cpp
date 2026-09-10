/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommonTasks.cpp

Abstract:

    Implementation of execution logic shared by multiple commands.

--*/
#include "Argument.h"
#include "CLIExecutionContext.h"
#include "CommonTasks.h"
#include "Exceptions.h"

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc::task {

// Prompts the user to confirm the action described by Data::ConfirmMessage, preceded by the
// Data::ConfirmWarning explaining what the action affects. The prompt is skipped when --force is
// passed, and declining terminates the invocation without running the remaining tasks.
void ConfirmAction(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::ConfirmMessage));

    if (context.Args.GetValue<ArgType::Force>())
    {
        return;
    }

    if (context.Data.Contains(Data::ConfirmWarning))
    {
        context.Terminal.Warn(L"{}\n", context.Data.Get<Data::ConfirmWarning>());
    }

    if (!context.Terminal.Confirm(context.Data.Get<Data::ConfirmMessage>()))
    {
        throw TerminateException{};
    }
}

} // namespace wsl::windows::wslc::task
