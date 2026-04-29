/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerSettings.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ContainerSettings class.

--*/

#include "precomp.h"
#include "ContainerSettings.h"
#include "Microsoft.WSL.Containers.ContainerSettings.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {

ContainerSettings::ContainerSettings(hstring const& imageName) :
    m_imageName(winrt::to_string(imageName))
{
    if (imageName.empty())
    {
        throw hresult_invalid_argument();
    }
}

hstring ContainerSettings::ImageName()
{
    return winrt::to_hstring(m_imageName);
}

void ContainerSettings::ImageName(hstring const& value)
{
    if (m_containerSettings)
    {
        throw hresult_illegal_state_change();
    }

    if (value.empty())
    {
        throw hresult_invalid_argument();
    }

    m_imageName = winrt::to_string(value);
}

hstring ContainerSettings::Name()
{
    return winrt::to_hstring(m_name);
}

void ContainerSettings::Name(hstring const& value)
{
    if (m_containerSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_name = winrt::to_string(value);
}

winrt::Microsoft::WSL::Containers::ProcessSettings ContainerSettings::InitProcess()
{
    return m_initProcess;
}

void ContainerSettings::InitProcess(winrt::Microsoft::WSL::Containers::ProcessSettings const& value)
{
    if (m_containerSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_initProcess = value;
}

winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::ContainerNetworkingMode> ContainerSettings::NetworkingMode()
{
    return m_networkingMode;
}

void ContainerSettings::NetworkingMode(winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::ContainerNetworkingMode> const& value)
{
    if (m_containerSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_networkingMode = value;
}

hstring ContainerSettings::HostName()
{
    return winrt::to_hstring(m_hostName);
}

void ContainerSettings::HostName(hstring const& value)
{
    if (m_containerSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_hostName = winrt::to_string(value);
}

hstring ContainerSettings::DomainName()
{
    return winrt::to_hstring(m_domainName);
}

void ContainerSettings::DomainName(hstring const& value)
{
    if (m_containerSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_domainName = winrt::to_string(value);
}

ContainerFlags ContainerSettings::Flags()
{
    return m_flags;
}

void ContainerSettings::Flags(ContainerFlags const& value)
{
    if (m_containerSettings)
    {
        throw hresult_illegal_state_change();
    }

    if (WI_IsFlagSet(value, ContainerFlags::Privileged))
    {
        throw hresult_not_implemented();
    }

    m_flags = value;
}

winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerPortMapping> ContainerSettings::PortMappings()
{
    return m_portMappings;
}

void ContainerSettings::PortMappings(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerPortMapping> const& value)
{
    if (m_containerSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_portMappings = value;
}

winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerVolume> ContainerSettings::Volumes()
{
    return m_volumes;
}

void ContainerSettings::Volumes(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerVolume> const& value)
{
    if (m_containerSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_volumes = value;
}

winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerNamedVolume> ContainerSettings::NamedVolumes()
{
    return m_namedVolumes;
}

void ContainerSettings::NamedVolumes(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerNamedVolume> const& value)
{
    if (m_containerSettings)
    {
        throw hresult_illegal_state_change();
    }

    m_namedVolumes = value;
}

WslcContainerSettings* ContainerSettings::ToStructPointer()
{
    if (!m_containerSettings)
    {
        m_containerSettings = std::make_unique<WslcContainerSettings>();
        winrt::check_hresult(WslcInitContainerSettings(m_imageName.c_str(), m_containerSettings.get()));

        if (!m_name.empty())
        {
            winrt::check_hresult(WslcSetContainerSettingsName(m_containerSettings.get(), m_name.c_str()));
        }

        if (m_initProcess)
        {
            winrt::check_hresult(WslcSetContainerSettingsInitProcess(m_containerSettings.get(), GetStructPointer(m_initProcess)));
        }

        if (m_networkingMode)
        {
            winrt::check_hresult(WslcSetContainerSettingsNetworkingMode(m_containerSettings.get(), static_cast<WslcContainerNetworkingMode>(m_networkingMode.Value())));
        }

        if (!m_hostName.empty())
        {
            winrt::check_hresult(WslcSetContainerSettingsHostName(m_containerSettings.get(), m_hostName.c_str()));
        }

        if (!m_domainName.empty())
        {
            winrt::check_hresult(WslcSetContainerSettingsDomainName(m_containerSettings.get(), m_domainName.c_str()));
        }

        winrt::check_hresult(WslcSetContainerSettingsFlags(m_containerSettings.get(), static_cast<WslcContainerFlags>(m_flags)));

        if (m_portMappings.Size() > 0)
        {
            m_portMappingsStructs.clear();
            m_portMappingsStructs.reserve(m_portMappings.Size());
            for (auto const& portMapping : m_portMappings)
            {
                m_portMappingsStructs.push_back(GetStruct(portMapping));
            }

            winrt::check_hresult(WslcSetContainerSettingsPortMappings(m_containerSettings.get(), m_portMappingsStructs.data(), static_cast<uint32_t>(m_portMappingsStructs.size())));
        }

        if (m_volumes.Size() > 0)
        {
            m_volumesStructs.clear();
            m_volumesStructs.reserve(m_volumes.Size());
            for (auto const& volume : m_volumes)
            {
                m_volumesStructs.push_back(GetStruct(volume));
            }

            winrt::check_hresult(WslcSetContainerSettingsVolumes(m_containerSettings.get(), m_volumesStructs.data(), static_cast<uint32_t>(m_volumesStructs.size())));
        }

        if (m_namedVolumes.Size() > 0)
        {
            m_namedVolumesStructs.clear();
            m_namedVolumesStructs.reserve(m_namedVolumes.Size());
            for (auto const& namedVolume : m_namedVolumes)
            {
                m_namedVolumesStructs.push_back(GetStruct(namedVolume));
            }

            winrt::check_hresult(WslcSetContainerSettingsNamedVolumes(m_containerSettings.get(), m_namedVolumesStructs.data(), static_cast<uint32_t>(m_namedVolumesStructs.size())));
        }
    }

    return m_containerSettings.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
