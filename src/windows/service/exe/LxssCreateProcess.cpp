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

// static function definitions
std::vector<gsl::byte> LxssCreateProcess::CreateMessage(_In_ LX_MESSAGE_TYPE MessageType, _In_ const CreateLxProcessData& CreateProcessData, _In_ ULONG DefaultUid)
{
    //
    // Compute the size of the total message starting with the base fields and
    // adding in the strings.
    //
    // N.B. The filename and command line are optional.
    //

    size_t MessageSize;
    switch (MessageType)
    {
    case LxInitMessageCreateProcess:
        MessageSize = offsetof(LX_INIT_CREATE_PROCESS, Common.Buffer);
        break;

    case LxInitMessageCreateProcessUtilityVm:
        MessageSize = offsetof(LX_INIT_CREATE_PROCESS_UTILITY_VM, Common.Buffer);
        break;

    default:
        THROW_HR(E_INVALIDARG);
    }

    THROW_IF_FAILED(SizeTAdd(CreateProcessData.Filename.length() + 1, MessageSize, &MessageSize));

    THROW_IF_FAILED(SizeTAdd(CreateProcessData.CurrentWorkingDirectory.length(), MessageSize, &MessageSize));

    THROW_IF_FAILED(SizeTAdd(1, MessageSize, &MessageSize));

    if (CreateProcessData.CommandLine.size() > 0)
    {
        for (size_t Index = 0; Index < CreateProcessData.CommandLine.size(); ++Index)
        {
            THROW_IF_FAILED(SizeTAdd(CreateProcessData.CommandLine[Index].length() + 1, MessageSize, &MessageSize));
        }
    }
    else
    {
        THROW_IF_FAILED(SizeTAdd(1, MessageSize, &MessageSize));
    }

    WI_ASSERT(CreateProcessData.Environment.size() > 0);

    for (size_t Index = 0; Index < CreateProcessData.Environment.size(); ++Index)
    {
        WI_ASSERT(CreateProcessData.Environment[Index].length() > 0);

        THROW_IF_FAILED(SizeTAdd(CreateProcessData.Environment[Index].length(), MessageSize, &MessageSize));

        THROW_IF_FAILED(SizeTAdd(1, MessageSize, &MessageSize));
    }

    if (CreateProcessData.NtEnvironment.size() > 0)
    {
        for (size_t Index = 0; Index < CreateProcessData.NtEnvironment.size(); ++Index)
        {
            WI_ASSERT(CreateProcessData.NtEnvironment[Index].length() > 0);

            THROW_IF_FAILED(SizeTAdd(CreateProcessData.NtEnvironment[Index].length(), MessageSize, &MessageSize));

            THROW_IF_FAILED(SizeTAdd(1, MessageSize, &MessageSize));
        }
    }
    else
    {
        THROW_IF_FAILED(SizeTAdd(1, MessageSize, &MessageSize));
    }

    THROW_IF_FAILED(SizeTAdd(CreateProcessData.NtPath.length(), MessageSize, &MessageSize));

    THROW_IF_FAILED(SizeTAdd(1, MessageSize, &MessageSize));

    THROW_IF_FAILED(SizeTAdd(CreateProcessData.Username.length(), MessageSize, &MessageSize));

    THROW_IF_FAILED(SizeTAdd(1, MessageSize, &MessageSize));

    //
    // Allocate the zero initialized buffer and populate the base fields.
    //

    THROW_HR_IF(E_INVALIDARG, MessageSize > ULONG_MAX);

    std::vector<gsl::byte> Message(MessageSize);
    const auto MessageSpan = gsl::make_span(Message);
    auto* MessageHeader = gslhelpers::get_struct<MESSAGE_HEADER>(MessageSpan);
    MessageHeader->MessageType = MessageType;
    MessageHeader->MessageSize = gsl::narrow_cast<ULONG>(MessageSize);
    gsl::span<gsl::byte> CommonSpan;
    if (MessageType == LxInitMessageCreateProcess)
    {
        CommonSpan = MessageSpan.subspan(offsetof(LX_INIT_CREATE_PROCESS, Common));
    }
    else
    {
        CommonSpan = MessageSpan.subspan(offsetof(LX_INIT_CREATE_PROCESS_UTILITY_VM, Common));
    }

    //
    // Populate the default UID.
    //

    auto* Common = gslhelpers::get_struct<LX_INIT_CREATE_PROCESS_COMMON>(CommonSpan);
    Common->DefaultUid = DefaultUid;

    //
    // Populate the Filename string.
    //

    size_t Offset = offsetof(LX_INIT_CREATE_PROCESS_COMMON, Buffer);
    Common->FilenameOffset = wsl::shared::string::CopyToSpan(CreateProcessData.Filename, CommonSpan, Offset);

    //
    // Populate the CurrentWorkingDirectory string.
    //
    // N.B. Checks for overflow were done earlier in this function.
    //

    Common->CurrentWorkingDirectoryOffset = wsl::shared::string::CopyToSpan(CreateProcessData.CurrentWorkingDirectory, CommonSpan, Offset);

    //
    // Populate the CommandLine strings.
    //

    WI_ASSERT(CreateProcessData.CommandLine.size() <= USHORT_MAX);

    Common->CommandLineOffset = gsl::narrow_cast<ULONG>(Offset);
    Common->CommandLineCount = gsl::narrow_cast<USHORT>(CreateProcessData.CommandLine.size());
    if (Common->CommandLineCount > 0)
    {
        for (USHORT Index = 0; Index < Common->CommandLineCount; ++Index)
        {
            wsl::shared::string::CopyToSpan(CreateProcessData.CommandLine[Index], CommonSpan, Offset);
        }
    }
    else
    {
        Offset += 1;
    }

    //
    // Populate the Environment strings.
    //

    WI_ASSERT(CreateProcessData.Environment.size() <= USHORT_MAX);

    Common->EnvironmentOffset = gsl::narrow_cast<ULONG>(Offset);
    Common->EnvironmentCount = gsl::narrow_cast<USHORT>(CreateProcessData.Environment.size());
    for (size_t Index = 0; Index < CreateProcessData.Environment.size(); ++Index)
    {
        wsl::shared::string::CopyToSpan(CreateProcessData.Environment[Index], CommonSpan, Offset);
    }

    //
    // Populate the NtEnvironment strings.
    //

    WI_ASSERT(CreateProcessData.NtEnvironment.size() <= USHORT_MAX);

    Common->NtEnvironmentOffset = gsl::narrow_cast<ULONG>(Offset);
    Common->NtEnvironmentCount = gsl::narrow_cast<USHORT>(CreateProcessData.NtEnvironment.size());
    if (Common->NtEnvironmentCount > 0)
    {
        for (USHORT Index = 0; Index < Common->NtEnvironmentCount; ++Index)
        {
            wsl::shared::string::CopyToSpan(CreateProcessData.NtEnvironment[Index], CommonSpan, Offset);
        }
    }
    else
    {
        Offset += 1;
    }

    //
    // Populate the shell options.
    //

    Common->ShellOptions = CreateProcessData.ShellOptions;

    //
    // Populate the NtPath string.
    //

    Common->NtPathOffset = wsl::shared::string::CopyToSpan(CreateProcessData.NtPath, CommonSpan, Offset);

    //
    // Populate the Username string.
    //

    Common->UsernameOffset = wsl::shared::string::CopyToSpan(CreateProcessData.Username, CommonSpan, Offset);

    WI_ASSERT(
        ((MessageType == LxInitMessageCreateProcess) && (MessageSize == (Offset + offsetof(LX_INIT_CREATE_PROCESS, Common)))) ||
        ((MessageType == LxInitMessageCreateProcessUtilityVm) &&
         (MessageSize == (Offset + offsetof(LX_INIT_CREATE_PROCESS_UTILITY_VM, Common)))));

    return Message;
}