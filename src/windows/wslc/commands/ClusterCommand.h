/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ClusterCommand.h

Abstract:

    Standalone developer cluster command declarations.

--*/
#pragma once
#include "Command.h"

namespace wsl::windows::wslc {

struct ClusterCommand final : public Command
{
    constexpr static std::wstring_view CommandName = L"cluster";
    ClusterCommand(const std::wstring& parent) : Command(CommandName, parent)
    {
    }

    std::wstring ShortDescription() const override;
    std::wstring LongDescription() const override;

protected:
    std::vector<std::unique_ptr<Command>> CreateCommands() const override;
    void ExecuteInternal(CLIExecutionContext& context) const override;
};

} // namespace wsl::windows::wslc
