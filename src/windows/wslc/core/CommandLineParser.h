/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommandLineParser.h

Abstract:

    Declaration of command-line resolution and scoped global option handling.

--*/
#pragma once

#include "Argument.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wsl::windows::wslc {
struct Command;
struct Invocation;

namespace execution {
    struct CLIExecutionContext;
}

struct GlobalArgumentScope
{
    std::wstring CommandInvocation;
    std::vector<Argument> Arguments;
};

class CommandTree
{
public:
    explicit CommandTree(std::unique_ptr<Command> root);
    ~CommandTree();

    CommandTree(const CommandTree&) = delete;
    CommandTree& operator=(const CommandTree&) = delete;
    CommandTree(CommandTree&&) noexcept;
    CommandTree& operator=(CommandTree&&) noexcept;

    const Command& Root() const;
    const Command& Selected() const;

private:
    void Select(const Command& command);

    std::unique_ptr<Command> m_root;
    std::optional<std::reference_wrapper<const Command>> m_selected;

    friend void ParseCommandLine(Invocation& invocation, execution::CLIExecutionContext& context, CommandTree& commandTree, bool applyEnvironmentOptions);
};

// Returns the global option scopes along target's path from the root.
std::vector<GlobalArgumentScope> GetGlobalArgumentPath(const Command& target);

// Parses scoped global options while selecting each command level in the persistent command tree.
void ParseCommandLine(Invocation& invocation, execution::CLIExecutionContext& context, CommandTree& commandTree, bool applyEnvironmentOptions = true);
} // namespace wsl::windows::wslc
