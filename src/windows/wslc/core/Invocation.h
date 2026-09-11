/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Invocation.h

Abstract:

    Declares the command-line cursor and invocation lifecycle.

    CommandInvocation owns the persistent command tree and mutable state for one invocation,
    including the original arguments, parser position, and selected command. CLIExecutionContext
    remains separate and owns the parsed values and execution services used by the selected command.

--*/
#pragma once

#include "defs.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace wsl::windows::wslc {
struct Argument;
struct Command;
struct CommandException;
struct Terminal;

enum class HelpOutput;

namespace argument {
    struct ArgMap;
}

namespace execution {
    struct CLIExecutionContext;
}

struct InvocationCursor
{
    InvocationCursor(std::vector<std::wstring>&& arguments) : m_arguments(std::move(arguments))
    {
    }

    struct iterator
    {
        iterator(size_t argument, const std::vector<std::wstring>& arguments) : m_argument(argument), m_arguments(arguments)
        {
        }

        iterator(const iterator&) = default;
        iterator& operator=(const iterator&) = default;

        iterator& operator++()
        {
            ++m_argument;
            return *this;
        }
        iterator operator++(int)
        {
            auto previous = *this;
            ++(*this);
            return previous;
        }
        iterator& operator--()
        {
            --m_argument;
            return *this;
        }
        iterator operator--(int)
        {
            auto previous = *this;
            --(*this);
            return previous;
        }

        bool operator==(const iterator& other) const
        {
            return m_argument == other.m_argument;
        }
        bool operator!=(const iterator& other) const
        {
            return m_argument != other.m_argument;
        }

        const std::wstring& operator*() const
        {
            return m_arguments[m_argument];
        }
        const std::wstring* operator->() const
        {
            return &m_arguments[m_argument];
        }

        size_t index() const
        {
            return m_argument;
        }

    private:
        size_t m_argument;
        const std::vector<std::wstring>& m_arguments;
    };

    size_t size() const
    {
        return m_arguments.size();
    }
    const std::vector<std::wstring>& OriginalArguments() const noexcept
    {
        return m_arguments;
    }
    size_t Position() const noexcept
    {
        return m_position;
    }
    iterator begin() const
    {
        return {m_position, m_arguments};
    }
    iterator end() const
    {
        return {m_arguments.size(), m_arguments};
    }
    void AdvancePast(const iterator& position)
    {
        m_position = position.index() + 1;
    }
    void SetPosition(const iterator& position)
    {
        m_position = position.index();
    }

private:
    std::vector<std::wstring> m_arguments;
    size_t m_position = 0;
};

// Owns command selection and parsing state for one CLI invocation. Execution state and services
// remain in CLIExecutionContext and are supplied when parsing or executing the selected command.
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

    void ApplyRootEnvironmentOptions(argument::ArgMap& arguments) const;
    void ParseCommandLine(execution::CLIExecutionContext& context);
    void Execute(execution::CLIExecutionContext& context) const;
    void OutputHelp(Terminal& terminal, HelpOutput output, const CommandException* exception = nullptr, std::span<const Argument> relevantArguments = {}) const;

private:
    void Select(const Command& command);

    std::unique_ptr<Command> m_root;
    InvocationCursor m_cursor;
    std::optional<std::reference_wrapper<const Command>> m_selected;
};
} // namespace wsl::windows::wslc
