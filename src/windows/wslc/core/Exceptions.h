/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Exceptions.h

Abstract:

    Header file for Exceptions.

--*/
#pragma once

namespace wsl::windows::wslc {
// Base exception for all command-related errors
struct CommandException
{
    CommandException(std::wstring_view message) : m_message(message)
    {
    }

    const std::wstring& Message() const
    {
        return m_message;
    }

protected:
    std::wstring m_message;
};

// Specific exception for argument parsing errors
struct ArgumentException : CommandException
{
    ArgumentException(std::wstring_view message) : CommandException(message)
    {
    }
};
} // namespace wsl::windows::wslc
