/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Exceptions.h

Abstract:

    Header file for Exceptions.

--*/
#pragma once
#include "pch.h"

namespace wsl::windows::wslc
{
    // Base exception for all command-related errors
    struct CommandException : std::exception
    {
        CommandException(std::wstring_view message) : m_message(message) {}

        const std::wstring& Message() const
        {
            return m_message;
        }

        const char* what() const noexcept override
        {
            static thread_local std::string buffer;
            buffer = wsl::windows::common::string::WideToMultiByte(m_message.c_str());
            return buffer.c_str();
        }

    protected:
        std::wstring m_message;
    };

    // Specific exception for argument parsing errors
    struct ArgumentException : CommandException
    {
        ArgumentException(std::wstring_view message) 
            : CommandException(message) {}
    };
}