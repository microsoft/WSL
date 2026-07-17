// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "GuestDeviceManager.h"
#include "DeviceHostProxy.h"

GuestDeviceManager::GuestDeviceManager(_In_ const std::wstring& machineId, _In_ const GUID& runtimeId, bool EnableTelemetry) :
    m_machineId(machineId), m_deviceHostSupport(wil::MakeOrThrow<DeviceHostProxy>(machineId, runtimeId, EnableTelemetry))
{
}

GuestDeviceManager::~GuestDeviceManager()
{
    try
    {
        m_deviceHostSupport->Shutdown();
    }
    CATCH_LOG()
}

_Requires_lock_not_held_(m_lock)
GUID GuestDeviceManager::AddVirtiofsDevice(_In_ PCWSTR Label, _In_opt_ PCWSTR MountOptions, _In_ PCWSTR RootPath, _In_ HANDLE UserToken, VirtioFsShareOptions Options)
{
    auto guestDeviceLock = m_lock.lock_exclusive();
    return m_deviceHostSupport->AddVirtiofsDevice(
        UserToken, Label, RootPath, Options.Kind, Options.SharedMemorySizeMb, MountOptions ? MountOptions : L"");
}

_Requires_lock_not_held_(m_lock)
GUID GuestDeviceManager::AddVirtioPmemDevice(_In_ PCWSTR Path, bool ReadOnly, _In_ HANDLE UserToken)
{
    auto guestDeviceLock = m_lock.lock_exclusive();
    return m_deviceHostSupport->AddVirtioPmemDevice(UserToken, Path, !ReadOnly);
}

_Requires_lock_not_held_(m_lock)
GUID GuestDeviceManager::AddNewDevice(_In_ const GUID& deviceId, _In_ const wil::com_ptr<IPlan9FileSystem>& server, _In_ PCWSTR tag)
{
    auto guestDeviceLock = m_lock.lock_exclusive();
    return m_deviceHostSupport->AddNewDevice(deviceId, server, tag);
}

GUID GuestDeviceManager::AddVirtioNetDevice(_In_ PCWSTR Tag, const WslVirtioNetConfig& Config, const std::vector<IpAddress>& Nameservers, _In_ HANDLE UserToken)
{
    auto guestDeviceLock = m_lock.lock_exclusive();
    THROW_HR_IF(E_INVALIDARG, m_virtioNetDevices.contains(Tag));
    const auto instanceId = m_deviceHostSupport->AddVirtioNetDevice(UserToken, Config, Nameservers);
    m_virtioNetDevices.emplace(Tag, instanceId);
    return instanceId;
}

wil::com_ptr<IWslVirtioNetDevice> GuestDeviceManager::GetVirtioNetDevice(_In_ PCWSTR Tag)
{
    auto guestDeviceLock = m_lock.lock_shared();
    const auto device = m_virtioNetDevices.find(Tag);
    THROW_HR_IF(E_NOT_SET, device == m_virtioNetDevices.end());
    return m_deviceHostSupport->GetVirtioNetDevice(device->second);
}

void GuestDeviceManager::AddRemoteFileSystem(_In_ REFCLSID clsid, _In_ PCWSTR tag, _In_ const wil::com_ptr<IPlan9FileSystem>& server)
{
    m_deviceHostSupport->AddRemoteFileSystem(clsid, tag, server);
}

void GuestDeviceManager::AddSharedMemoryDevice(_In_ PCWSTR Tag, _In_ PCWSTR Path, _In_ UINT32 SizeMb, _In_ HANDLE UserToken)
{
    auto guestDeviceLock = m_lock.lock_exclusive();
    auto objectLifetime = CreateSectionObjectRoot(Path, UserToken);

    (void)m_deviceHostSupport->AddVirtiofsDevice(
        UserToken, Tag, objectLifetime.Path, VirtiofsShareKind_SectionBacked, SizeMb, L"");
    m_objectDirectories.emplace_back(std::move(objectLifetime));
}

GuestDeviceManager::DirectoryObjectLifetime GuestDeviceManager::CreateSectionObjectRoot(_In_ std::wstring_view RelativeRootPath, _In_ HANDLE UserToken) const
{
    auto revert = wil::impersonate_token(UserToken);
    DWORD sessionId;
    DWORD bytesWritten;
    THROW_LAST_ERROR_IF(!GetTokenInformation(GetCurrentThreadToken(), TokenSessionId, &sessionId, sizeof(sessionId), &bytesWritten));

    // /Sessions/1/BaseNamedObjects/WSL/<VM ID>/<Relative Path>
    std::wstringstream sectionPathBuilder;
    sectionPathBuilder << L"\\Sessions\\" << sessionId << L"\\BaseNamedObjects" << L"\\WSL\\" << m_machineId << L"\\" << RelativeRootPath;
    auto sectionPath = sectionPathBuilder.str();

    UNICODE_STRING ntPath{};
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(OBJECT_ATTRIBUTES);
    attributes.ObjectName = &ntPath;
    std::vector<wil::unique_handle> directoryHierarchy;
    auto remainingPath = std::wstring_view(sectionPath.data(), sectionPath.length());
    while (remainingPath.length() > 0)
    {
        // Find the next path substring, ignoring the root path backslash.
        auto nextDir = remainingPath;
        const auto separatorPos = nextDir.find(L"\\", remainingPath[0] == L'\\' ? 1 : 0);
        if (separatorPos != std::wstring_view::npos)
        {
            nextDir = nextDir.substr(0, separatorPos);
            remainingPath = remainingPath.substr(separatorPos + 1, std::wstring_view::npos);

            // Skip concurrent backslashes.
            while (remainingPath.length() > 0 && remainingPath[0] == L'\\')
            {
                remainingPath = remainingPath.substr(1, std::wstring_view::npos);
            }
        }
        else
        {
            remainingPath = remainingPath.substr(remainingPath.length(), std::wstring_view::npos);
        }

        attributes.RootDirectory = directoryHierarchy.size() > 0 ? directoryHierarchy.back().get() : nullptr;
        ntPath.Buffer = const_cast<PWCH>(nextDir.data());
        ntPath.Length = sizeof(WCHAR) * gsl::narrow_cast<USHORT>(nextDir.length());
        ntPath.MaximumLength = ntPath.Length;
        wil::unique_handle nextHandle;
        NTSTATUS status = ZwCreateDirectoryObject(&nextHandle, DIRECTORY_ALL_ACCESS, &attributes);
        if (status == STATUS_OBJECT_NAME_COLLISION)
        {
            status = NtOpenDirectoryObject(&nextHandle, MAXIMUM_ALLOWED, &attributes);
        }
        THROW_IF_NTSTATUS_FAILED(status);
        directoryHierarchy.emplace_back(std::move(nextHandle));
    }

    return {std::move(sectionPath), std::move(directoryHierarchy)};
}

wil::com_ptr<IPlan9FileSystem> GuestDeviceManager::GetRemoteFileSystem(_In_ REFCLSID clsid, _In_ std::wstring_view tag)
{
    return m_deviceHostSupport->GetRemoteFileSystem(clsid, tag);
}

void GuestDeviceManager::SetSwiotlb(UINT64 GpaBase, UINT64 SizeBytes)
{
    m_deviceHostSupport->SetSwiotlb(GpaBase, SizeBytes);
}

_Requires_lock_not_held_(m_lock)
void GuestDeviceManager::RemoveGuestDevice(_In_ const GUID& InstanceId)
{
    auto guestDeviceLock = m_lock.lock_exclusive();
    for (auto it = m_virtioNetDevices.begin(); it != m_virtioNetDevices.end(); ++it)
    {
        if (IsEqualGUID(it->second, InstanceId))
        {
            m_virtioNetDevices.erase(it);
            break;
        }
    }

    m_deviceHostSupport->RemoveDevice(InstanceId);
}
