/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerStateStore.cpp

Abstract:

    Contains the implementation for WSLCContainerStateStore.

--*/

#include "precomp.h"
#include "WSLCContainerStateStore.h"
#include "WSLCSessionDefaults.h"
#include "WSLCSessionRuntime.h"

using wsl::windows::service::wslc::WSLCContainerStateStorage;
using wsl::windows::service::wslc::WSLCContainerStateStore;
using wsl::windows::service::wslc::WSLCPersistedContainerState;
using wsl::windows::service::wslc::WSLCSessionRuntime;
using wsl::windows::service::wslc::WSLCVirtualMachine;

namespace {

constexpr auto c_documentName = "wslc-container-state.json";

// Documents hold a handful of counters, so anything larger is not one of ours.
constexpr size_t c_maxDocumentSize = 64 * 1024;

std::string ContainerStatePath(const std::string& ContainerId)
{
    return std::format("{}/containers/{}/{}", wsl::windows::wslc::ContainerdStorageMountPoint, ContainerId, c_documentName);
}

// Callers hold a VM lease while writing or loading, so this uses the VM directly.
WSLCVirtualMachine& Vm(WSLCSessionRuntime& Runtime)
{
    THROW_HR_IF(WSLC_E_VM_NOT_RUNNING, !Runtime.HasVm());
    return Runtime.Vm();
}

WSLCPersistedContainerState LoadContainerState(WSLCSessionRuntime& Runtime, const std::string& Path)
{
    const auto content = Vm(Runtime).ReadGuestFile(Path, c_maxDocumentSize);
    return wsl::shared::FromJson<WSLCPersistedContainerState>(content.begin(), content.end());
}

HRESULT WriteContainerState(WSLCSessionRuntime& Runtime, const std::string& Path, const WSLCPersistedContainerState& State) noexcept
try
{
    Vm(Runtime).WriteGuestFile(Path, wsl::shared::ToJson(State), WSLCWriteFileFlagsAtomic | WSLCWriteFileFlagsNoCreateParents);
    return S_OK;
}
CATCH_RETURN()

} // namespace

WSLCPersistedContainerState WSLCContainerStateStore::ForNewContainer(WSLCSessionRuntime& Runtime, const std::string& ContainerId)
{
    WSLCPersistedContainerState state;

    // The metadata label records that the document exists, so the container is not created without it.
    if (const auto result = WriteContainerState(Runtime, ContainerStatePath(ContainerId), state); FAILED(result))
    {
        THROW_HR_WITH_USER_ERROR_MSG(
            result,
            wsl::shared::Localization::MessageWslcContainerStateSaveFailed(wsl::shared::string::MultiByteToWide(ContainerId)),
            "Failed to save the persisted state of container '%hs'",
            ContainerId.c_str());
    }

    return state;
}

WSLCPersistedContainerState WSLCContainerStateStore::ForExistingContainer(WSLCSessionRuntime& Runtime, const std::string& ContainerId, WSLCContainerStateStorage Storage)
{
    const auto path = ContainerStatePath(ContainerId);
    switch (Storage)
    {
    case WSLCContainerStateStorage::None:
        // The container predates persisted state, so its document only exists once its state has been written.
        try
        {
            return LoadContainerState(Runtime, path);
        }
        catch (const wil::ResultException& e)
        {
            if (e.GetErrorCode() != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            {
                throw;
            }

            return {};
        }

    case WSLCContainerStateStorage::GuestFileV1:
        return LoadContainerState(Runtime, path);

    default:
        THROW_HR_MSG(HRESULT_FROM_WIN32(ERROR_UNSUPPORTED_TYPE), "Unsupported state storage: %d", static_cast<int>(Storage));
    }
}

WSLCContainerStateStore::WSLCContainerStateStore(
    WSLCSessionRuntime& Runtime, const std::string& ContainerId, const WSLCPersistedContainerState& State, HRESULT LoadError) :
    m_runtime(Runtime), m_path(ContainerStatePath(ContainerId)), m_loadError(LoadError), m_state(State)
{
}

WSLCPersistedContainerState WSLCContainerStateStore::Get() const
{
    std::lock_guard lock{m_lock};
    return m_state;
}

HRESULT WSLCContainerStateStore::WriteLockHeld() noexcept
{
    if (FAILED(m_loadError))
    {
        return m_loadError;
    }

    const auto result = WriteContainerState(m_runtime, m_path, m_state);
    m_writeFailed = FAILED(result);
    return result;
}