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

ContainerSettings::ContainerSettings(hstring const& imageName) : m_imageName(winrt::to_string(imageName))
{
    if (imageName.empty())
    {
        throw winrt::hresult_invalid_argument(L"Image name cannot be empty");
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
        throw winrt::hresult_illegal_state_change(L"Cannot change image name after container has been initialized");
    }

    if (value.empty())
    {
        throw winrt::hresult_invalid_argument(L"Image name cannot be empty");
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
        throw winrt::hresult_illegal_state_change(L"Cannot change container name after container has been initialized");
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
        throw winrt::hresult_illegal_state_change(L"Cannot change init process after container has been initialized");
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
        throw winrt::hresult_illegal_state_change(L"Cannot change networking mode after container has been initialized");
    }

    if (value && value.Value() != ContainerNetworkingMode::None && value.Value() != ContainerNetworkingMode::Bridged)
    {
        throw winrt::hresult_invalid_argument(L"Invalid networking mode");
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
        throw winrt::hresult_illegal_state_change(L"Cannot change host name after container has been initialized");
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
        throw winrt::hresult_illegal_state_change(L"Cannot change domain name after container has been initialized");
    }

    m_domainName = winrt::to_string(value);
}

bool ContainerSettings::EnableAutoRemove()
{
    return WI_IsFlagSet(m_containerFlags, WSLC_CONTAINER_FLAG_AUTO_REMOVE);
}

void ContainerSettings::EnableAutoRemove(bool value)
{
    if (m_containerSettings)
    {
        throw winrt::hresult_illegal_state_change(L"Cannot change container settings after container has been initialized");
    }

    WI_UpdateFlag(m_containerFlags, WSLC_CONTAINER_FLAG_AUTO_REMOVE, value);
}

bool ContainerSettings::EnableGpu()
{
    return WI_IsFlagSet(m_containerFlags, WSLC_CONTAINER_FLAG_ENABLE_GPU);
}

void ContainerSettings::EnableGpu(bool value)
{
    if (m_containerSettings)
    {
        throw winrt::hresult_illegal_state_change(L"Cannot change container settings after container has been initialized");
    }

    WI_UpdateFlag(m_containerFlags, WSLC_CONTAINER_FLAG_ENABLE_GPU, value);
}

bool ContainerSettings::Privileged()
{
    return WI_IsFlagSet(m_containerFlags, WSLC_CONTAINER_FLAG_PRIVILEGED);
}

void ContainerSettings::Privileged(bool value)
{
    if (m_containerSettings)
    {
        throw winrt::hresult_illegal_state_change(L"Cannot change container settings after container has been initialized");
    }

    WI_UpdateFlag(m_containerFlags, WSLC_CONTAINER_FLAG_PRIVILEGED, value);
}

winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerPortMapping> ContainerSettings::PortMappings()
{
    return m_portMappings;
}

void ContainerSettings::PortMappings(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerPortMapping> const& value)
{
    if (m_containerSettings)
    {
        throw winrt::hresult_illegal_state_change(L"Cannot change port mappings after container has been initialized");
    }

    if (!value)
    {
        throw winrt::hresult_error(E_POINTER, L"Value cannot be null");
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
        throw winrt::hresult_illegal_state_change(L"Cannot change volumes after container has been initialized");
    }

    if (!value)
    {
        throw winrt::hresult_error(E_POINTER, L"Value cannot be null");
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
        throw winrt::hresult_illegal_state_change(L"Cannot change named volumes after container has been initialized");
    }

    if (!value)
    {
        throw winrt::hresult_error(E_POINTER, L"Value cannot be null");
    }

    m_namedVolumes = value;
}

winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerNetwork> ContainerSettings::Networks()
{
    return m_networks;
}

void ContainerSettings::Networks(winrt::Windows::Foundation::Collections::IVector<winrt::Microsoft::WSL::Containers::ContainerNetwork> const& value)
{
    if (m_containerSettings)
    {
        throw winrt::hresult_illegal_state_change(L"Cannot change networks after container has been initialized");
    }

    if (!value)
    {
        throw winrt::hresult_error(E_POINTER, L"Value cannot be null");
    }

    m_networks = value;
}

winrt::Windows::Foundation::Collections::IVector<hstring> ContainerSettings::CapabilityAdditions()
{
    return m_capabilityAdditions;
}

void ContainerSettings::CapabilityAdditions(winrt::Windows::Foundation::Collections::IVector<hstring> const& value)
{
    if (m_containerSettings)
    {
        throw winrt::hresult_illegal_state_change(L"Cannot change capabilities after container has been initialized");
    }

    if (!value)
    {
        winrt::throw_hresult(E_POINTER);
    }

    m_capabilityAdditions = value;
}

winrt::Windows::Foundation::Collections::IVector<hstring> ContainerSettings::CapabilityDrops()
{
    return m_capabilityDrops;
}

void ContainerSettings::CapabilityDrops(winrt::Windows::Foundation::Collections::IVector<hstring> const& value)
{
    if (m_containerSettings)
    {
        winrt::throw_hresult(E_ILLEGAL_STATE_CHANGE);
    }

    if (!value)
    {
        winrt::throw_hresult(E_POINTER);
    }

    m_capabilityDrops = value;
}

WslcContainerSettings* ContainerSettings::ToStructPointer()
{
    if (m_containerSettings)
    {
        return m_containerSettings.get();
    }

    // Cache the settings only after every conversion succeeds so a failed attempt is not reused by a later call.
    auto settings = std::make_unique<WslcContainerSettings>();
    winrt::check_hresult(WslcInitContainerSettings(m_imageName.c_str(), settings.get()));

    if (!m_name.empty())
    {
        winrt::check_hresult(WslcSetContainerSettingsName(settings.get(), m_name.c_str()));
    }

    if (m_initProcess)
    {
        winrt::check_hresult(WslcSetContainerSettingsInitProcess(settings.get(), GetStructPointer(m_initProcess)));
    }

    if (m_networkingMode)
    {
        winrt::check_hresult(WslcSetContainerSettingsNetworkingMode(
            settings.get(), static_cast<WslcContainerNetworkingMode>(m_networkingMode.Value())));
    }

    if (!m_hostName.empty())
    {
        winrt::check_hresult(WslcSetContainerSettingsHostName(settings.get(), m_hostName.c_str()));
    }

    if (!m_domainName.empty())
    {
        winrt::check_hresult(WslcSetContainerSettingsDomainName(settings.get(), m_domainName.c_str()));
    }

    winrt::check_hresult(WslcSetContainerSettingsFlags(settings.get(), m_containerFlags));

    if (m_portMappings.Size() > 0)
    {
        m_portMappingsStructs.clear();
        m_portMappingsStructs.reserve(m_portMappings.Size());
        for (auto const& portMapping : m_portMappings)
        {
            if (!portMapping)
            {
                throw winrt::hresult_error(E_POINTER, L"Port mappings collection contains a null element");
            }
            m_portMappingsStructs.push_back(GetStruct(portMapping));
        }

        winrt::check_hresult(WslcSetContainerSettingsPortMappings(
            settings.get(), m_portMappingsStructs.data(), static_cast<uint32_t>(m_portMappingsStructs.size())));
    }

    if (m_volumes.Size() > 0)
    {
        m_volumesStructs.clear();
        m_volumesStructs.reserve(m_volumes.Size());
        for (auto const& volume : m_volumes)
        {
            if (!volume)
            {
                throw winrt::hresult_error(E_POINTER, L"Volumes collection contains a null element");
            }
            m_volumesStructs.push_back(GetStruct(volume));
        }

        winrt::check_hresult(
            WslcSetContainerSettingsVolumes(settings.get(), m_volumesStructs.data(), static_cast<uint32_t>(m_volumesStructs.size())));
    }

    if (m_namedVolumes.Size() > 0)
    {
        m_namedVolumesStructs.clear();
        m_namedVolumesStructs.reserve(m_namedVolumes.Size());
        for (auto const& namedVolume : m_namedVolumes)
        {
            if (!namedVolume)
            {
                throw winrt::hresult_error(E_POINTER, L"Named volumes collection contains a null element");
            }
            m_namedVolumesStructs.push_back(GetStruct(namedVolume));
        }

        winrt::check_hresult(WslcSetContainerSettingsNamedVolumes(
            settings.get(), m_namedVolumesStructs.data(), static_cast<uint32_t>(m_namedVolumesStructs.size())));
    }

    if (m_networks.Size() > 0)
    {
        // Hold references to the applied networks since m_networksStructs points into them and the Networks
        // collection can still be modified by the caller.
        m_appliedNetworks.clear();
        m_appliedNetworks.reserve(m_networks.Size());
        m_networksStructs.clear();
        m_networksStructs.reserve(m_networks.Size());
        for (auto const& network : m_networks)
        {
            if (!network)
            {
                throw winrt::hresult_error(E_POINTER, L"Networks collection contains a null element");
            }
            m_appliedNetworks.push_back(network);
            m_networksStructs.push_back(GetStruct(network));
        }

        winrt::check_hresult(WslcSetContainerSettingsNetworks(
            settings.get(), m_networksStructs.data(), static_cast<uint32_t>(m_networksStructs.size())));
    }

    if (m_capabilityAdditions.Size() > 0)
    {
        m_capabilityAdditionStrings.clear();
        m_capabilityAdditionStrings.reserve(m_capabilityAdditions.Size());
        for (const auto& capability : m_capabilityAdditions)
        {
            m_capabilityAdditionStrings.push_back(winrt::to_string(capability));
        }

        m_capabilityAdditionPointers.clear();
        m_capabilityAdditionPointers.reserve(m_capabilityAdditionStrings.size());
        for (const auto& capability : m_capabilityAdditionStrings)
        {
            m_capabilityAdditionPointers.push_back(capability.c_str());
        }

        winrt::check_hresult(WslcSetContainerSettingsCapabilityAdditions(
            settings.get(), m_capabilityAdditionPointers.data(), static_cast<uint32_t>(m_capabilityAdditionPointers.size())));
    }

    if (m_capabilityDrops.Size() > 0)
    {
        m_capabilityDropStrings.clear();
        m_capabilityDropStrings.reserve(m_capabilityDrops.Size());
        for (const auto& capability : m_capabilityDrops)
        {
            m_capabilityDropStrings.push_back(winrt::to_string(capability));
        }

        m_capabilityDropPointers.clear();
        m_capabilityDropPointers.reserve(m_capabilityDropStrings.size());
        for (const auto& capability : m_capabilityDropStrings)
        {
            m_capabilityDropPointers.push_back(capability.c_str());
        }

        winrt::check_hresult(WslcSetContainerSettingsCapabilityDrops(
            settings.get(), m_capabilityDropPointers.data(), static_cast<uint32_t>(m_capabilityDropPointers.size())));
    }

    m_containerSettings = std::move(settings);
    return m_containerSettings.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
