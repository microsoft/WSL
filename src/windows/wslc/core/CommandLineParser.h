/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommandLineParser.h

Abstract:

    Declaration of command-line resolution and scoped global option handling.

--*/
#pragma once

#include "Argument.h"
#include "Invocation.h"
#include "defs.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wsl::windows::wslc {
struct Command;

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

    NON_COPYABLE(CommandTree);

    CommandTree(CommandTree&&) noexcept;
    CommandTree& operator=(CommandTree&&) noexcept;

    const Command& Root() const;

private:
    std::unique_ptr<Command> m_root;
};

class CommandInvocation
{
public:
    CommandInvocation(std::unique_ptr<Command> root, std::vector<std::wstring>&& arguments);
    ~CommandInvocation();

    NON_COPYABLE(CommandInvocation);

    CommandInvocation(CommandInvocation&& other) noexcept;
    CommandInvocation& operator=(CommandInvocation&& other) noexcept;

    const Command& Root() const;
    const Command& Selected() const;
    const std::vector<std::wstring>& OriginalArguments() const noexcept;
    size_t Position() const noexcept;

private:
    InvocationCursor& Cursor() noexcept;
    void Select(const Command& command);

    CommandTree m_commands;
    InvocationCursor m_cursor;
    std::optional<std::reference_wrapper<const Command>> m_selected;

    friend void ParseCommandLine(CommandInvocation& invocation, execution::CLIExecutionContext& context);
};

// Returns the global option scopes along target's path from the root.
std::vector<GlobalArgumentScope> GetGlobalArgumentPath(const Command& target);

// Parses scoped global options while selecting each command level in the persistent command tree.
void ParseCommandLine(CommandInvocation& invocation, execution::CLIExecutionContext& context);
} // namespace wsl::windows::wslc
