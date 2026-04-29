/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionTasks.cpp

Abstract:

    Implementation of version command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentValidation.h"
#include "VersionTasks.h"
#include "VersionService.h"
#include "CLIExecutionContext.h"
#include "TableOutput.h"
#include <wil/result_macros.h>
#include <wslc_schema.h>

using namespace wsl::shared;
using namespace wsl::shared::string;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;
using namespace wsl::windows::common::wslutil;

namespace wsl::windows::wslc::task {

void GetVersionInfo(CLIExecutionContext& context)
{
    context.Data.Add<Data::Version>(VersionService::VersionInfo());
}

void ListVersionInfo(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Version));
    auto& versionInfo = context.Data.Get<Data::Version>();

    FormatType format = FormatType::Table; // Default is table
    if (context.Args.Contains(ArgType::Format))
    {
        format = validation::GetFormatTypeFromString(context.Args.Get<ArgType::Format>());
    }

    switch (format)
    {
    case FormatType::Json:
    {
        auto json = ToJson(versionInfo, c_jsonPrettyPrintIndent);
        PrintMessage(MultiByteToWide(json));
        break;
    }
    case FormatType::Table:
    {
        constexpr auto c_indent = L"  ";
        auto table = wsl::windows::wslc::TableOutput<2>({L"", L""});
        table.SetShowHeader(false);

        // Client section
        table.OutputLine({Localization::WSLCCLI_VersionTableClientHeader(), L""});
        table.OutputLine({c_indent + Localization::WSLCCLI_VersionTableVersion(), MultiByteToWide(versionInfo.Client.Version)});
        table.OutputLine({c_indent + Localization::WSLCCLI_VersionTableGitCommit(), MultiByteToWide(versionInfo.Client.GitCommit)});
        table.OutputLine({c_indent + Localization::WSLCCLI_VersionTableBuilt(), MultiByteToWide(versionInfo.Client.Built)});
        table.OutputLine(
            {c_indent + Localization::WSLCCLI_VersionTableOsArch(), MultiByteToWide(versionInfo.Client.Os + "/" + versionInfo.Client.Arch)});

        // Blank separator
        table.OutputLine({L"", L""});

        // Server section
        table.OutputLine({Localization::WSLCCLI_VersionTableServerHeader(), L""});
        table.OutputLine({c_indent + Localization::WSLCCLI_VersionTableLinuxKernel(), MultiByteToWide(versionInfo.Server.Kernel)});
        table.OutputLine({c_indent + Localization::WSLCCLI_VersionTableWSLg(), MultiByteToWide(versionInfo.Server.WSLg)});
        table.OutputLine({c_indent + Localization::WSLCCLI_VersionTableMSRDC(), MultiByteToWide(versionInfo.Server.MSRDC)});
        table.OutputLine({c_indent + Localization::WSLCCLI_VersionTableDirect3D(), MultiByteToWide(versionInfo.Server.Direct3D)});
        table.OutputLine({c_indent + Localization::WSLCCLI_VersionTableDXCore(), MultiByteToWide(versionInfo.Server.DXCore)});
        table.OutputLine({c_indent + Localization::WSLCCLI_VersionTableWindows(), MultiByteToWide(versionInfo.Server.Windows)});

        table.Complete();
        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
}
} // namespace wsl::windows::wslc::task
