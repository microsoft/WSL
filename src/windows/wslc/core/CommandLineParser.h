/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommandLineParser.h

Abstract:

    Declaration of command-line resolution and scoped global option handling.

--*/
#pragma once

#include "Argument.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wsl::windows::wslc {
struct Command;
struct Invocation;

namespace execution {
    struct CLIExecutionContext;
}

struct GlobalArgumentScope
{
    std::wstring CommandFullName;
    std::wstring CommandInvocation;
    std::vector<Argument> Arguments;
};

// Returns every command scope in the tree rooted at root that defines global options.
std::vector<GlobalArgumentScope> GetGlobalArgumentScopes(const Command& root);

// Returns the global option scopes inherited along the path from root to commandFullName.
std::vector<GlobalArgumentScope> GetGlobalArgumentPath(const Command& root, std::wstring_view commandFullName);

// Parses scoped global options while resolving each command level, then parses the selected command.
// The command reference is updated as each subcommand is selected so callers can report errors against
// the command scope where they occurred.
void ParseCommandLine(Invocation& invocation, execution::CLIExecutionContext& context, std::unique_ptr<Command>& command, bool applyEnvironmentOptions = true);
} // namespace wsl::windows::wslc
