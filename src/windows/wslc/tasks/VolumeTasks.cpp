/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeTasks.cpp

Abstract:

    Implementation of volume command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentConvertedTypes.h"
#include "CLIExecutionContext.h"
#include "VolumeModel.h"
#include "VolumeService.h"
#include "VolumeTasks.h"
#include "TableOutput.h"
#include <wslc_schema.h>

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {

static bool TryInspectVolume(Reporter& reporter, Session& session, const std::string& volumeName, std::optional<wslc_schema::InspectVolume>& inspectData)
{
    try
    {
        inspectData = VolumeService::Inspect(session, volumeName);
        return true;
    }
    catch (const wil::ResultException& ex)
    {
        if (ex.GetErrorCode() == WSLC_E_VOLUME_NOT_FOUND)
        {
            reporter.Error(L"{}\n", Localization::MessageWslcVolumeNotFound(volumeName.c_str()));
            return false;
        }

        throw;
    }
}

static bool TryDeleteVolume(Reporter& reporter, Session& session, const std::string& volumeName, bool force)
{
    try
    {
        VolumeService::Delete(session, volumeName);
        return true;
    }
    catch (const wil::ResultException& ex)
    {
        if (ex.GetErrorCode() == WSLC_E_VOLUME_NOT_FOUND)
        {
            if (!force)
            {
                reporter.Error(L"{}\n", Localization::MessageWslcVolumeNotFound(volumeName.c_str()));
            }

            return false;
        }

        throw;
    }
}

void CreateVolume(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));

    models::CreateVolumeOptions options{};
    if (context.Args.Contains(ArgType::VolumeName))
    {
        options.Name = WideToMultiByte(context.Args.GetValue<ArgType::VolumeName>());
    }

    for (const auto& option : context.Args.GetAllValues<ArgType::Options>())
    {
        options.DriverOpts.push_back(option);
    }

    for (const auto& label : context.Args.GetAllValues<ArgType::Label>())
    {
        options.Labels.push_back(label);
    }

    if (context.Args.Contains(ArgType::Driver))
    {
        options.Driver = WideToMultiByte(context.Args.GetValue<ArgType::Driver>());
    }

    auto result = VolumeService::Create(context.Data.Get<Data::Session>(), options);
    context.Reporter.Output(L"{}\n", MultiByteToWide(result.Name));
}

void DeleteVolumes(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto volumeNames = context.Args.GetAllValues<ArgType::VolumeName>();
    const bool force = context.Args.GetFlag<ArgType::Force>();
    for (const auto& name : volumeNames)
    {
        if (TryDeleteVolume(context.Reporter, session, WideToMultiByte(name), force))
        {
            context.Reporter.Output(L"{}\n", name);
        }
        else if (!force)
        {
            context.ExitCode = 1;
        }
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
    auto volumeNames = context.Args.GetAllValues<ArgType::VolumeName>();
    std::vector<wsl::windows::common::wslc_schema::InspectVolume> result;
    for (const auto& name : volumeNames)
    {
        std::optional<wslc_schema::InspectVolume> inspectData;
        if (TryInspectVolume(context.Reporter, session, WideToMultiByte(name), inspectData))
        {
            result.push_back(*inspectData);
        }
        else
        {
            context.ExitCode = 1;
        }
    }

    auto json = ToJson(result, c_jsonPrettyPrintIndent);
    context.Reporter.Output(L"{}\n", MultiByteToWide(json));
}

void ListVolumes(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Volumes));
    auto& volumes = context.Data.Get<Data::Volumes>();

    if (context.Args.GetFlag<ArgType::Quiet>())
    {
        for (const auto& volume : volumes)
        {
            context.Reporter.Output(L"{}\n", MultiByteToWide(volume.Name));
        }

        return;
    }

    FormatType format = FormatType::Table;
    if (context.Args.Contains(ArgType::Format))
    {
        format = context.Args.GetValue<ArgType::Format>();
    }

    switch (format)
    {
    case FormatType::Json:
    {
        auto json = ToJson(volumes, c_jsonPrettyPrintIndent);
        context.Reporter.Output(L"{}\n", MultiByteToWide(json));
        break;
    }
    case FormatType::Table:
    {
        auto table = wsl::windows::wslc::TableOutput<2>(context.Reporter, {L"DRIVER", L"VOLUME NAME"});
        for (const auto& volume : volumes)
        {
            table.WriteRow({
                MultiByteToWide(volume.Driver),
                MultiByteToWide(volume.Name),
            });
        }

        table.Complete();
        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

void PruneVolumes(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    const bool all = context.Args.GetFlag<ArgType::All>();

    // Filter values are parsed and cached during argument validation.
    auto filters = context.Args.GetAllValues<ArgType::Filter>();

    auto result = VolumeService::Prune(context.Reporter, session, all, filters);

    for (const auto& volumeName : result.PrunedVolumes)
    {
        context.Reporter.Output(L"{}\n", Localization::WSLCCLI_VolumePruneDeleted(MultiByteToWide(volumeName)));
    }

    context.Reporter.Output(L"\n");
    context.Reporter.Output(L"{}\n", Localization::WSLCCLI_VolumePruneSpaceReclaimed(wsl::shared::string::FormatBytes(result.SpaceReclaimed)));
}
} // namespace wsl::windows::wslc::task
