/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeTasks.cpp

Abstract:

    Implementation of volume command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentValidation.h"
#include "CLIExecutionContext.h"
#include "VolumeModel.h"
#include "VolumeService.h"
#include "VolumeTasks.h"
#include "TableOutput.h"
#include <wslc_schema.h>

using namespace wsl::shared;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {

void CreateVolume(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::VolumeName));

    auto name = WideToMultiByte(context.Args.Get<ArgType::VolumeName>());
    std::string type = "vhd";
    if (context.Args.Contains(ArgType::VolumeDriver))
    {
        type = WideToMultiByte(context.Args.Get<ArgType::VolumeDriver>());
    }

    VolumeService::Create(context.Data.Get<Data::Session>(), name, type);
    PrintMessage(MultiByteToWide(name));
}

void DeleteVolumes(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto volumeNames = context.Args.GetAll<ArgType::VolumeName>();
    for (const auto& name : volumeNames)
    {
        VolumeService::Delete(session, WideToMultiByte(name));
    }
}

void GetVolumes(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    context.Data.Add<Data::Volumes>(VolumeService::List(session));
}

void InspectVolumes(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto volumeNames = context.Args.GetAll<ArgType::VolumeName>();
    std::vector<wsl::windows::common::wslc_schema::InspectVolume> result;
    for (const auto& name : volumeNames)
    {
        auto inspectData = VolumeService::Inspect(session, WideToMultiByte(name));
        result.push_back(inspectData);
    }

    auto json = ToJson(result, c_jsonPrettyPrintIndent);
    PrintMessage(MultiByteToWide(json));
}

void ListVolumes(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Volumes));
    auto& volumes = context.Data.Get<Data::Volumes>();

    if (context.Args.Contains(ArgType::Quiet))
    {
        for (const auto& volume : volumes)
        {
            PrintMessage(MultiByteToWide(volume.Name));
        }

        return;
    }

    FormatType format = FormatType::Table;
    if (context.Args.Contains(ArgType::Format))
    {
        format = validation::GetFormatTypeFromString(context.Args.Get<ArgType::Format>());
    }

    switch (format)
    {
    case FormatType::Json:
    {
        auto json = ToJson(volumes, c_jsonPrettyPrintIndent);
        PrintMessage(MultiByteToWide(json));
        break;
    }
    case FormatType::Table:
    {
        auto table = wsl::windows::wslc::TableOutput<2>({L"VOLUME NAME", L"DRIVER"});
        for (const auto& volume : volumes)
        {
            table.OutputLine({
                MultiByteToWide(volume.Name),
                MultiByteToWide(volume.Type),
            });
        }

        table.Complete();
        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

} // namespace wsl::windows::wslc::task
