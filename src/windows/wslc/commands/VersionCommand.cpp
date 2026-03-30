/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionCommand.cpp

Abstract:

    Implementation of the version command, which displays detailed version information
    similar to `docker version`.

--*/
#include "ArgumentValidation.h"
#include "CLIExecutionContext.h"
#include "ContainerModel.h"
#include "VersionCommand.h"
#include "helpers.hpp"
#include <nlohmann/json.hpp>

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::shared::string;

namespace wsl::windows::wslc {

namespace {
std::string GetArchString()
{
#if defined(_M_ARM64)
    return "arm64";
#elif defined(_M_X64)
    return "amd64";
#elif defined(_M_IX86)
    return "386";
#else
    return "unknown";
#endif
}
} // namespace

std::vector<Argument> VersionCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Format),
    };
}

std::wstring VersionCommand::ShortDescription() const
{
    return {L"Show detailed version information."};
}

std::wstring VersionCommand::LongDescription() const
{
    return {L"Displays detailed version information for wslc and its components, including build metadata and system details."};
}

void VersionCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersionString();
    const auto arch = GetArchString();

    FormatType format = FormatType::Table;
    if (context.Args.Contains(ArgType::Format))
    {
        format = validation::GetFormatTypeFromString(context.Args.Get<ArgType::Format>());
    }

    switch (format)
    {
    case FormatType::Json:
    {
        nlohmann::json client;
        client["Platform"]["Name"] = "Windows Subsystem for Linux";
        client["Version"] = std::string(WSL_PACKAGE_VERSION);
        client["GitCommit"] = std::string(COMMIT_HASH);
        client["Os"] = "windows";
        client["Arch"] = arch;
        client["BuildTime"] = std::string(__DATE__) + " " + __TIME__;
        client["Compiler"] = "MSVC " + std::to_string(_MSC_VER);

        nlohmann::json components = nlohmann::json::array();

        nlohmann::json kernel;
        kernel["Name"] = "Kernel";
        kernel["Version"] = std::string(KERNEL_VERSION);
        components.push_back(kernel);

        nlohmann::json wslg;
        wslg["Name"] = "WSLg";
        wslg["Version"] = std::string(WSLG_VERSION);
        components.push_back(wslg);

        nlohmann::json d3d;
        d3d["Name"] = "Direct3D";
        d3d["Version"] = std::string(DIRECT3D_VERSION);
        components.push_back(d3d);

        nlohmann::json dxcore;
        dxcore["Name"] = "DXCore";
        dxcore["Version"] = std::string(DXCORE_VERSION);
        components.push_back(dxcore);

        client["Components"] = components;

        if (!windowsVersion.empty())
        {
            client["WindowsVersion"] = windowsVersion;
        }

        nlohmann::json root;
        root["Client"] = client;
        wsl::windows::common::wslutil::PrintMessage(MultiByteToWide(root.dump(1)), stdout);
        break;
    }
    case FormatType::Table:
    default:
    {
        std::wstring output = std::format(
            L"Client:\n"
            L" Version:    {}\n"
            L" OS/Arch:    windows/{}\n"
            L" Git commit: {}\n"
            L" Kernel:     {}\n"
            L" WSLg:       {}\n"
            L" Direct3D:   {}\n"
            L" DXCore:     {}",
            WSL_PACKAGE_VERSION,
            MultiByteToWide(arch),
            COMMIT_HASH,
            KERNEL_VERSION,
            WSLG_VERSION,
            DIRECT3D_VERSION,
            DXCORE_VERSION);

        if (!windowsVersion.empty())
        {
            output += std::format(L"\n Windows:    {}", MultiByteToWide(windowsVersion));
        }

        if constexpr (!wsl::shared::OfficialBuild)
        {
            output += std::format(L"\n Built:      {} {}", __DATE__, __TIME__);
            output += std::format(L"\n Compiler:   MSVC {}", _MSC_VER);
        }

        wsl::windows::common::wslutil::PrintMessage(output, stdout);
        break;
    }
    }
}
} // namespace wsl::windows::wslc
