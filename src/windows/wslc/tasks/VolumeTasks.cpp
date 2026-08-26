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
using wsl::windows::common::string::FormatHumanReadableSize;

namespace wsl::windows::wslc::task {

namespace {

    // Reported for the fields that only carry a value when volume usage data or swarm cluster
    // information is available, neither of which applies here.
    constexpr std::string_view c_volumeNotAvailable = "N/A";

    // Converts session volume entries into the all-string shape used for "volume list --format json".
    VolumeOutputInformation ToVolumeOutput(const wslc_schema::VolumeListEntry& volume)
    {
        VolumeOutputInformation entry;
        entry.Availability = c_volumeNotAvailable;
        entry.Driver = volume.Driver;
        entry.Group = c_volumeNotAvailable;
        entry.Links = c_volumeNotAvailable;
        entry.Mountpoint = volume.Mountpoint;
        entry.Name = volume.Name;
        entry.Scope = volume.Scope;
        entry.Size = c_volumeNotAvailable;
        entry.Status = c_volumeNotAvailable;

        for (const auto& [key, value] : volume.Labels)
        {
            if (!entry.Labels.empty())
            {
                entry.Labels += ",";
            }

            entry.Labels += std::format("{}={}", key, value);
        }

        return entry;
    }

} // namespace

static bool TryInspectVolume(Terminal& terminal, Session& session, const std::string& volumeName, std::optional<wslc_schema::InspectVolume>& inspectData)
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
            terminal.Error(L"{}\n", Localization::MessageWslcVolumeNotFound(volumeName.c_str()));
            return false;
        }

        throw;
    }
}

static bool TryDeleteVolume(Terminal& terminal, Session& session, const std::string& volumeName, bool force)
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
                terminal.Error(L"{}\n", Localization::MessageWslcVolumeNotFound(volumeName.c_str()));
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
    context.Terminal.Output(L"{}\n", MultiByteToWide(result.Name));
}

void DeleteVolumes(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto volumeNames = context.Args.GetAllValues<ArgType::VolumeName>();
    const bool force = context.Args.GetValue<ArgType::Force>();
    for (const auto& name : volumeNames)
    {
        if (TryDeleteVolume(context.Terminal, session, WideToMultiByte(name), force))
        {
            context.Terminal.Output(L"{}\n", name);
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
    auto filters = context.Args.GetAllValues<ArgType::Filter>();
    context.Data.Add<Data::Volumes>(VolumeService::List(session, filters));
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
        if (TryInspectVolume(context.Terminal, session, WideToMultiByte(name), inspectData))
        {
            result.push_back(*inspectData);
        }
        else
        {
            context.ExitCode = 1;
        }
    }

    auto json = ToJson(result, context.Args.GetValue<ArgType::InspectFormat>(c_jsonPrettyPrintIndent));
    context.Terminal.Output(L"{}\n", MultiByteToWide(json));
}

void ListVolumes(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Volumes));
    auto& volumes = context.Data.Get<Data::Volumes>();

    if (context.Args.GetValue<ArgType::Quiet>())
    {
        for (const auto& volume : volumes)
        {
            context.Terminal.Output(L"{}\n", MultiByteToWide(volume.Name));
        }

        return;
    }

    const auto format = context.Args.GetValue<ArgType::Format>(FormatType::Table);

    switch (format)
    {
    case FormatType::Json:
    {
        for (const auto& volume : volumes)
        {
            context.Terminal.Output(L"{}\n", ToJsonW(ToVolumeOutput(volume), c_jsonCompactIndent));
        }

        break;
    }
    case FormatType::Table:
    {
        auto table = wsl::windows::wslc::TableOutput<2>(context.Terminal, {L"DRIVER", L"VOLUME NAME"});
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

    const bool all = context.Args.GetValue<ArgType::All>();

    // Filter values are parsed and cached during argument validation.
    auto filters = context.Args.GetAllValues<ArgType::Filter>();

    auto result = VolumeService::Prune(context.Terminal, session, all, filters);

    if (!result.PrunedVolumes.empty())
    {
        context.Terminal.Output(L"{}\n", Localization::WSLCCLI_VolumePruneDeletedHeader());
        for (const auto& volumeName : result.PrunedVolumes)
        {
            context.Terminal.Output(L"{}\n", MultiByteToWide(volumeName));
        }

        context.Terminal.Output(L"\n");
    }

    context.Terminal.Output(
        L"{}\n", Localization::WSLCCLI_VolumePruneSpaceReclaimed(FormatHumanReadableSize(result.SpaceReclaimed, c_reclaimedSpacePrecision)));
}
} // namespace wsl::windows::wslc::task
