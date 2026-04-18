/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionTasks.cpp

Abstract:

    Implementation of version command related execution logic.

--*/
#include "precomp.h"
#include "Argument.h"
#include "ArgumentValidation.h"
#include "ContainerModel.h"
#include "VersionTasks.h"
#include "VersionService.h"
#include "CLIExecutionContext.h"
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
        PrintMessage(MultiByteToWide(versionInfo.ToString()));
        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
}
} // namespace wsl::windows::wslc::task
