// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "RootCommand.h"
#include <winrt/Windows.System.Profile.h>

//// Include all commands here so they output in help appropriately.
#include "TestCommand.h"

////#include "TableOutput.h"

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc
{
    namespace 
    {
        std::wstring GetOSVersion()
        {
            using namespace winrt::Windows::System::Profile;
            auto versionInfo = AnalyticsInfo::VersionInfo();

            uint64_t version = std::stoull(versionInfo.DeviceFamilyVersion().c_str());
            uint16_t parts[4];

            for (size_t i = 0; i < ARRAYSIZE(parts); ++i)
            {
                parts[i] = version & 0xFFFF;
                version = version >> 16;
            }

            std::wostringstream strstr;
            strstr << versionInfo.DeviceFamily().c_str() << L" v" << parts[3] << L'.' << parts[2] << L'.' << parts[1] << L'.' << parts[0];
            return strstr.str();
        }
    }

    std::vector<std::unique_ptr<Command>> RootCommand::GetCommands() const
    {
        return InitializeFromMoveOnly<std::vector<std::unique_ptr<Command>>>({
            std::make_unique<TestCommand>(FullName()),
        });
    }

    std::vector<Argument> RootCommand::GetArguments() const
    {
        return
        {
            Argument{ Args::Type::Info, L"List information for the tool", ArgumentType::Flag, Argument::Visibility::Help },
        };
    }

    std::wstring_view RootCommand::ShortDescription() const
    {
        return { L"WSLC is the Windows Subsystem for Linux Container CLI tool." };
    }

    std::wstring_view RootCommand::LongDescription() const
    {
        return { L"WSLC is the Windows Subsystem for Linux Container CLI tool. It enables management and interaction with WSL containers from the command line." };
    }

    void RootCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        std::wostringstream info;
        info << L"Windows: " << GetOSVersion();
        PrintMessage(info.str(), stdout);
    }
}

