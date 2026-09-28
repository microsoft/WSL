/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssCreateProcess.cpp

Abstract:

    This file contains process creation function definitions.

--*/

#include "precomp.h"
#include "LxssCreateProcess.h"

CreateLxProcessData LxssCreateProcess::ParseArguments(
    _In_opt_ LPCSTR Filename,
    _In_ ULONG CommandLineCount,
    _In_reads_opt_(CommandLineCount) LPCSTR* CommandLine,
    _In_opt_ LPCWSTR CurrentWorkingDirectory,
    _In_opt_ LPCWSTR NtPath,
    _In_reads_opt_(NtEnvironmentLength) PWCHAR NtEnvironment,
    _In_ ULONG NtEnvironmentLength,
    _In_opt_ LPCWSTR Username,
    _In_ const std::vector<std::string>& DefaultEnvironment,
    _In_ ULONG Flags)
{
    THROW_HR_IF(E_INVALIDARG, (!ARGUMENT_PRESENT(Filename) && (CommandLineCount > 1)));
    THROW_HR_IF(E_INVALIDARG, ((CommandLineCount != 0) && !CommandLine) || (CommandLineCount > USHORT_MAX));

    // Convert the input strings to counted strings that reuse the existing
    // buffer so the length of the strings is only computed once.

    CreateLxProcessData Parsed{};
    if (ARGUMENT_PRESENT(Filename))
    {
        Parsed.Filename = Filename;
        THROW_HR_IF(E_INVALIDARG, Parsed.Filename.empty());
        Parsed.CommandLine.reserve(CommandLineCount);
    }
    else if (CommandLineCount > 0)
    {
        Parsed.CommandLine.reserve(CommandLineCount + 1);
        Parsed.CommandLine.emplace_back(std::string("-c"));
    }

    for (size_t Index = 0; Index < CommandLineCount; ++Index)
    {
        Parsed.CommandLine.emplace_back(std::string(CommandLine[Index]));
    }

    // Initialize the environment.

    Parsed.Environment = DefaultEnvironment;

    // Append the user's NT path if the configuration supports it.
    //
    // N.B. Failures to append user's NT path are non-fatal and errors are
    //      logged internally.

    if ((ARGUMENT_PRESENT(NtPath)) && (LXSS_INTEROP_ENABLED(Flags)) && (WI_IsFlagSet(Flags, LXSS_DISTRO_FLAGS_APPEND_NT_PATH)))
    {
        Parsed.NtPath = wsl::shared::string::WideToMultiByte(NtPath);
    }

    // Validate that the environment is a NUL-NUL-terminated string.

    if (ARGUMENT_PRESENT(NtEnvironment))
    {
        for (size_t Index = 0;;)
        {
            const PWCHAR Current = NtEnvironment + Index;
            const size_t Length = wcsnlen(Current, NtEnvironmentLength - Index);
            THROW_HR_IF(E_INVALIDARG, Length == NtEnvironmentLength - Index);
            if (Length == 0)
            {
                break;
            }

            Parsed.NtEnvironment.push_back(wsl::shared::string::WideToMultiByte(Current));
            Index += Length + 1;
        }
    }

    // Translate the username to UTF-8.

    if (ARGUMENT_PRESENT(Username))
    {
        Parsed.Username = wsl::shared::string::WideToMultiByte(Username);
    }

    // Initialize the current working directory.
    //
    // N.B. An empty current working directory means the user's home path will
    //      be used.

    if (ARGUMENT_PRESENT(CurrentWorkingDirectory))
    {
        Parsed.CurrentWorkingDirectory = wsl::shared::string::WideToMultiByte(CurrentWorkingDirectory);
    }

    return Parsed;
}
