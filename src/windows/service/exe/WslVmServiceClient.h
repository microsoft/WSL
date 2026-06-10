// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    WslVmServiceClient.h

Abstract:

    Thin C++ wrapper around wslvmservice.dll.

    wslvmservice.dll (built from the hvlite repo) exports a plain C ABI that
    drives an OpenVMM VM over ttrpc. This wrapper loads the DLL by full path,
    resolves the exports, and exposes them as member functions that return
    HRESULT, so the rest of WSLC can call it like a normal object.

    The Rust side owns an opaque handle; this class creates one on construction
    and destroys it on teardown.

--*/

#pragma once

#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <wil/resource.h>
#include <wil/result.h>

namespace wsl::windows::service::wslc {

class WslVmServiceClient
{
public:
    WslVmServiceClient()
    {
        // Load the DLL by full path from the directory of the running module to
        // avoid DLL search-order hijacking. wslvmservice.dll is installed
        // alongside wslservice.exe.
        wchar_t modulePath[MAX_PATH]{};
        THROW_LAST_ERROR_IF(GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath)) == 0);

        auto dllPath = std::filesystem::path(modulePath).parent_path() / L"wslvmservice.dll";
        m_module.reset(LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
        THROW_LAST_ERROR_IF_MSG(!m_module, "Failed to load wslvmservice.dll from %ls", dllPath.c_str());

        m_create = GetProc<WslVmServiceCreateFn>("WslVmServiceCreate");
        m_destroy = GetProc<WslVmServiceDestroyFn>("WslVmServiceDestroy");
        m_connect = GetProc<ConnectFn>("WslVmServiceConnect");
        m_disconnect = GetProc<HandleOnlyFn>("WslVmServiceDisconnect");
        m_setKernelPath = GetProc<StringFn>("WslVmServiceSetKernelPath");
        m_setInitrdPath = GetProc<StringFn>("WslVmServiceSetInitrdPath");
        m_setKernelCmdLine = GetProc<StringFn>("WslVmServiceSetKernelCmdLine");
        m_setMemoryMb = GetProc<SetMemoryMbFn>("WslVmServiceSetMemoryMb");
        m_setProcessorCount = GetProc<SetU32Fn>("WslVmServiceSetProcessorCount");
        m_setHvSocketPath = GetProc<StringFn>("WslVmServiceSetHvSocketPath");
        m_addBootDisk = GetProc<DiskFn>("WslVmServiceAddBootDisk");
        m_setConsommeNic = GetProc<TwoStringFn>("WslVmServiceSetConsommeNic");
        m_addSerialPort = GetProc<SerialPortFn>("WslVmServiceAddSerialPort");
        m_setVirtioConsolePath = GetProc<StringFn>("WslVmServiceSetVirtioConsolePath");
        m_createVm = GetProc<HandleOnlyFn>("WslVmServiceCreateVm");
        m_resumeVm = GetProc<HandleOnlyFn>("WslVmServiceResumeVm");
        m_teardownVm = GetProc<HandleOnlyFn>("WslVmServiceTeardownVm");
        m_attachScsiDisk = GetProc<DiskFn>("WslVmServiceAttachScsiDisk");
        m_detachScsiDisk = GetProc<DetachScsiDiskFn>("WslVmServiceDetachScsiDisk");
        m_addShare = GetProc<AddShareFn>("WslVmServiceAddShare");
        m_removeShare = GetProc<StringFn>("WslVmServiceRemoveShare");
        m_bindPort = GetProc<PortFn>("WslVmServiceBindPort");
        m_unbindPort = GetProc<PortFn>("WslVmServiceUnbindPort");

        THROW_IF_FAILED_MSG(m_create(&m_handle), "Failed to create wslvmservice client");
    }

    ~WslVmServiceClient()
    {
        if (m_handle != nullptr)
        {
            m_destroy(m_handle);
            m_handle = nullptr;
        }
    }

    WslVmServiceClient(const WslVmServiceClient&) = delete;
    WslVmServiceClient& operator=(const WslVmServiceClient&) = delete;
    WslVmServiceClient(WslVmServiceClient&&) = delete;
    WslVmServiceClient& operator=(WslVmServiceClient&&) = delete;

    HRESULT Connect(LPCWSTR socketPath, UINT32 timeoutMs) const
    {
        return m_connect(m_handle, socketPath, timeoutMs);
    }

    HRESULT Disconnect() const
    {
        return m_disconnect(m_handle);
    }

    HRESULT SetKernelPath(LPCWSTR path) const
    {
        return m_setKernelPath(m_handle, path);
    }

    HRESULT SetInitrdPath(LPCWSTR path) const
    {
        return m_setInitrdPath(m_handle, path);
    }

    HRESULT SetKernelCmdLine(LPCWSTR cmdline) const
    {
        return m_setKernelCmdLine(m_handle, cmdline);
    }

    HRESULT SetMemoryMb(UINT64 memoryMb) const
    {
        return m_setMemoryMb(m_handle, memoryMb);
    }

    HRESULT SetProcessorCount(UINT32 count) const
    {
        return m_setProcessorCount(m_handle, count);
    }

    HRESULT SetHvSocketPath(LPCWSTR path) const
    {
        return m_setHvSocketPath(m_handle, path);
    }

    HRESULT AddBootDisk(UINT32 controller, UINT32 lun, LPCWSTR hostPath, BOOL readOnly) const
    {
        return m_addBootDisk(m_handle, controller, lun, hostPath, readOnly);
    }

    HRESULT SetConsommeNic(LPCWSTR nicId, LPCWSTR macAddress) const
    {
        return m_setConsommeNic(m_handle, nicId, macAddress);
    }

    HRESULT AddSerialPort(UINT32 port, LPCWSTR socketPath) const
    {
        return m_addSerialPort(m_handle, port, socketPath);
    }

    HRESULT SetVirtioConsolePath(LPCWSTR path) const
    {
        return m_setVirtioConsolePath(m_handle, path);
    }

    HRESULT CreateVm() const
    {
        return m_createVm(m_handle);
    }

    HRESULT ResumeVm() const
    {
        return m_resumeVm(m_handle);
    }

    HRESULT TeardownVm() const
    {
        return m_teardownVm(m_handle);
    }

    HRESULT AttachScsiDisk(UINT32 controller, UINT32 lun, LPCWSTR hostPath, BOOL readOnly) const
    {
        return m_attachScsiDisk(m_handle, controller, lun, hostPath, readOnly);
    }

    HRESULT DetachScsiDisk(UINT32 controller, UINT32 lun) const
    {
        return m_detachScsiDisk(m_handle, controller, lun);
    }

    HRESULT AddShare(LPCWSTR tag, LPCWSTR hostPath, BOOL readOnly) const
    {
        return m_addShare(m_handle, tag, hostPath, readOnly);
    }

    HRESULT RemoveShare(LPCWSTR tag) const
    {
        return m_removeShare(m_handle, tag);
    }

    HRESULT BindPort(UINT16 hostPort, UINT16 guestPort, BOOL tcp, INT32 family) const
    {
        return m_bindPort(m_handle, hostPort, guestPort, tcp, family);
    }

    HRESULT UnbindPort(UINT16 hostPort, UINT16 guestPort, BOOL tcp, INT32 family) const
    {
        return m_unbindPort(m_handle, hostPort, guestPort, tcp, family);
    }

private:
    using WslVmServiceCreateFn = HRESULT(__cdecl*)(void** handle);
    using WslVmServiceDestroyFn = void(__cdecl*)(void* handle);
    using HandleOnlyFn = HRESULT(__cdecl*)(void* handle);
    using ConnectFn = HRESULT(__cdecl*)(void* handle, LPCWSTR socketPath, UINT32 timeoutMs);
    using StringFn = HRESULT(__cdecl*)(void* handle, LPCWSTR value);
    using TwoStringFn = HRESULT(__cdecl*)(void* handle, LPCWSTR a, LPCWSTR b);
    using SetMemoryMbFn = HRESULT(__cdecl*)(void* handle, UINT64 value);
    using SetU32Fn = HRESULT(__cdecl*)(void* handle, UINT32 value);
    using DiskFn = HRESULT(__cdecl*)(void* handle, UINT32 controller, UINT32 lun, LPCWSTR hostPath, BOOL readOnly);
    using DetachScsiDiskFn = HRESULT(__cdecl*)(void* handle, UINT32 controller, UINT32 lun);
    using SerialPortFn = HRESULT(__cdecl*)(void* handle, UINT32 port, LPCWSTR socketPath);
    using AddShareFn = HRESULT(__cdecl*)(void* handle, LPCWSTR tag, LPCWSTR hostPath, BOOL readOnly);
    using PortFn = HRESULT(__cdecl*)(void* handle, UINT16 hostPort, UINT16 guestPort, BOOL tcp, INT32 family);

    template <typename TFn>
    TFn GetProc(const char* name) const
    {
        auto proc = reinterpret_cast<TFn>(GetProcAddress(m_module.get(), name));
        THROW_LAST_ERROR_IF_MSG(proc == nullptr, "wslvmservice.dll missing export: %hs", name);
        return proc;
    }

    wil::unique_hmodule m_module;
    void* m_handle = nullptr;

    WslVmServiceCreateFn m_create = nullptr;
    WslVmServiceDestroyFn m_destroy = nullptr;
    ConnectFn m_connect = nullptr;
    HandleOnlyFn m_disconnect = nullptr;
    StringFn m_setKernelPath = nullptr;
    StringFn m_setInitrdPath = nullptr;
    StringFn m_setKernelCmdLine = nullptr;
    SetMemoryMbFn m_setMemoryMb = nullptr;
    SetU32Fn m_setProcessorCount = nullptr;
    StringFn m_setHvSocketPath = nullptr;
    DiskFn m_addBootDisk = nullptr;
    TwoStringFn m_setConsommeNic = nullptr;
    SerialPortFn m_addSerialPort = nullptr;
    StringFn m_setVirtioConsolePath = nullptr;
    HandleOnlyFn m_createVm = nullptr;
    HandleOnlyFn m_resumeVm = nullptr;
    HandleOnlyFn m_teardownVm = nullptr;
    DiskFn m_attachScsiDisk = nullptr;
    DetachScsiDiskFn m_detachScsiDisk = nullptr;
    AddShareFn m_addShare = nullptr;
    StringFn m_removeShare = nullptr;
    PortFn m_bindPort = nullptr;
    PortFn m_unbindPort = nullptr;
};

} // namespace wsl::windows::service::wslc
