/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InspectCommand.h

Abstract:

    Declaration of the root-level InspectCommand, which supports --type to inspect
    containers or images (matching Docker's `docker inspect --type` behavior).

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {
struct InspectCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"inspect";
    InspectCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }
    std::vector<Argument> GetArguments() const override;
    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    void ExecuteInternal(CLIExecutionContext& context) const override;
};
} // namespace wsl::windows::wslc
