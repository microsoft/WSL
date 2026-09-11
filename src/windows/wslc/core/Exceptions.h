/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Exceptions.h

Abstract:

    Header file for Exceptions.

--*/
#pragma once

#include "Argument.h"

#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

namespace wsl::windows::wslc {
struct CLIException
{
    CLIException(std::wstring_view message) : m_message(message)
    {
    }

    const std::wstring& Message() const
    {
        return m_message;
    }

protected:
    std::wstring m_message;
};

// Specific exception for command parsing errors
struct CommandException : CLIException
{
    CommandException(std::wstring_view message) : CLIException(message)
    {
    }
};

// Specific exception for argument parsing errors
struct ArgumentException : CommandException
{
    ArgumentException(std::wstring_view message) : CommandException(message)
    {
    }

    ArgumentException(std::wstring_view message, Argument argument) : CommandException(message), m_arguments{std::move(argument)}
    {
    }

    ArgumentException(std::wstring_view message, std::vector<Argument> arguments) :
        CommandException(message), m_arguments(std::move(arguments))
    {
    }

    static ArgumentException CreateUnknownOption(std::wstring_view message, std::wstring_view token)
    {
        ArgumentException exception{message};
        exception.m_unknownOptionToken = token;
        return exception;
    }

    const std::vector<Argument>& Arguments() const
    {
        return m_arguments;
    }

    const std::optional<std::wstring>& UnknownOptionToken() const
    {
        return m_unknownOptionToken;
    }

private:
    std::vector<Argument> m_arguments;
    std::optional<std::wstring> m_unknownOptionToken;
};

// Specific exception for failures after command and argument validation
struct ExecutionException : CLIException
{
    ExecutionException(std::wstring_view message) : CLIException(message)
    {
    }
};

// Specific exception for a user declining a confirmation prompt. The requested action is abandoned
// and the program exits successfully.
struct TerminateException : CLIException
{
    TerminateException() : CLIException(std::wstring_view{})
    {
    }
};
} // namespace wsl::windows::wslc
