// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "GuestDeviceManager.h"
#include "DeviceHostProxy.h"

GuestDeviceManager::GuestDeviceManager(_In_ const std::wstring& machineId, _In_ const GUID& runtimeId) :
    m_machineId(machineId), m_deviceHostSupport(wil::MakeOrThrow<DeviceHostProxy>(machineId, runtimeId))
{
}

_Requires_lock_not_held_(m_lock)
GUID GuestDeviceManager::AddGuestDevice(
    _In_ const GUID& DeviceId, _In_ const GUID& ImplementationClsid, _In_ PCWSTR AccessName, _In_opt_ PCWSTR Options, _In_ PCWSTR Path, _In_ UINT32 Flags, _In_ HANDLE UserToken)
{
    auto guestDeviceLock = m_lock.lock_exclusive();
    return AddHdvShareWithOptions(DeviceId, ImplementationClsid, AccessName, Options, Path, Flags, UserToken);
}

_Requires_lock_held_(m_lock)
GUID GuestDeviceManager::AddHdvShareWithOptions(
    _In_ const GUID& DeviceId, _In_ const GUID& ImplementationClsid, _In_ PCWSTR AccessName, _In_opt_ PCWSTR Options, _In_ PCWSTR Path, _In_ UINT32 Flags, _In_ HANDLE UserToken)
{
    wil::com_ptr<IPlan9FileSystem> server;

    // Options are appended to the name with a semi-colon separator.
    //  "name;key1=value1;key2=value2"
    // The AddSharePath implementation is responsible for separating them out and interpreting them.
    std::wstring nameWithOptions{AccessName};
    if (ARGUMENT_PRESENT(Options))
    {
        nameWithOptions += L";";
        nameWithOptions += Options;
    }

    {
        auto revert = wil::impersonate_token(UserToken);

        server = GetRemoteFileSystem(ImplementationClsid, c_defaultDeviceTag);
        if (!server)
        {
            server = wil::CoCreateInstance<IPlan9FileSystem>(ImplementationClsid, (CLSCTX_LOCAL_SERVER | CLSCTX_ENABLE_CLOAKING | CLSCTX_ENABLE_AAA));
            AddRemoteFileSystem(ImplementationClsid, c_defaultDeviceTag.c_str(), server);
        }

        THROW_IF_FAILED(server->AddSharePath(nameWithOptions.c_str(), Path, Flags));
    }

    // This requires more privileges than the user may have, so impersonation is disabled.
    return AddNewDevice(DeviceId, server, AccessName);
}

GUID GuestDeviceManager::AddNewDevice(_In_ const GUID& deviceId, _In_ const wil::com_ptr<IPlan9FileSystem>& server, _In_ PCWSTR tag)
{
    return m_deviceHostSupport->AddNewDevice(deviceId, server, tag);
}

void GuestDeviceManager::AddRemoteFileSystem(_In_ REFCLSID clsid, _In_ PCWSTR tag, _In_ const wil::com_ptr<IPlan9FileSystem>& server)
{
    m_deviceHostSupport->AddRemoteFileSystem(clsid, tag, server);
}

void GuestDeviceManager::AddSharedMemoryDevice(_In_ const GUID& ImplementationClsid, _In_ PCWSTR Tag, _In_ PCWSTR Path, _In_ UINT32 SizeMb, _In_ HANDLE UserToken)
{
    auto guestDeviceLock = m_lock.lock_exclusive();
    auto objectLifetime = CreateSectionObjectRoot(Path, UserToken);

    // For virtiofs hdv, the flags parameter has been overloaded. Flags are placed in the lower
    // 16 bits, while the shared memory size in megabytes are placed in the upper 16 bits.
    static constexpr auto VIRTIO_FS_FLAGS_SHMEM_SIZE_SHIFT = 16;
    UINT32 flags = (SizeMb << VIRTIO_FS_FLAGS_SHMEM_SIZE_SHIFT);
    WI_SetFlag(flags, VIRTIO_FS_FLAGS_TYPE_SECTIONS);
    (void)AddHdvShareWithOptions(VIRTIO_FS_DEVICE_ID, ImplementationClsid, Tag, {}, objectLifetime.Path.c_str(), flags, UserToken);
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

void GuestDeviceManager::Shutdown()
try
{
    m_deviceHostSupport->Shutdown();
}
CATCH_LOG()
