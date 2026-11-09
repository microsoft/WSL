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

constexpr auto c_documentName = "wslc-state.json";

// Documents hold a handful of counters, so anything larger is not one of ours.
constexpr size_t c_maxDocumentSize = 64 * 1024;

enum class LoadStatus
{
    Loaded,
    Missing,
    Corrupt,
    Unreadable,
};

struct LoadResult
{
    LoadStatus Status{};
    WSLCPersistedContainerState State;
    HRESULT Error = S_OK;
};

LoadResult LoadFailure(LoadStatus Status, HRESULT Error) noexcept
{
    return {.Status = Status, .Error = Error};
}

LoadResult Corrupt(const std::string& Path, const char* Reason) noexcept
{
    WSL_LOG("ContainerStateCorrupt", TraceLoggingValue(Path.c_str(), "Path"), TraceLoggingValue(Reason, "Reason"));
    return LoadFailure(LoadStatus::Corrupt, HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT));
}

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

LoadResult LoadContainerState(WSLCSessionRuntime& Runtime, const std::string& Path) noexcept
try
{
    std::string content;
    const auto error = Vm(Runtime).ReadGuestFile(Path, c_maxDocumentSize, content);
    switch (error)
    {
    case 0:
        break;
    case ENOENT:
        return LoadFailure(LoadStatus::Missing, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    case EFBIG:
        return Corrupt(Path, "Document too large");
    default:
        WSL_LOG("ContainerStateUnreadable", TraceLoggingValue(Path.c_str(), "Path"), TraceLoggingValue(error, "Errno"));
        return LoadFailure(LoadStatus::Unreadable, E_FAIL);
    }

    try
    {
        return {.Status = LoadStatus::Loaded, .State = wsl::shared::FromJson<WSLCPersistedContainerState>(content.begin(), content.end())};
    }
    catch (const wil::ResultException& e)
    {
        if (e.GetErrorCode() != WSL_E_INVALID_JSON)
        {
            throw;
        }

        return Corrupt(Path, "Invalid JSON");
    }
}
catch (...)
{
    return LoadFailure(LoadStatus::Unreadable, wil::ResultFromCaughtException());
}

HRESULT WriteContainerState(WSLCSessionRuntime& Runtime, const std::string& Path, const WSLCPersistedContainerState& State) noexcept
try
{
    const auto error =
        Vm(Runtime).WriteGuestFile(Path, wsl::shared::ToJson(State), WSLCWriteFileFlagsAtomic | WSLCWriteFileFlagsNoCreateParents);
    RETURN_HR_IF_MSG(E_FAIL, error != 0, "Failed to write '%hs', errno: %d", Path.c_str(), error);

    return S_OK;
}
CATCH_RETURN()

} // namespace

WSLCContainerStateStore::Initial WSLCContainerStateStore::ForNewContainer(WSLCSessionRuntime& Runtime, const std::string& ContainerId)
{
    Initial initial{.Runtime = Runtime, .Path = ContainerStatePath(ContainerId)};

    // The metadata label records that the document exists, so the container is not created without it.
    if (const auto result = WriteContainerState(initial.Runtime, initial.Path, initial.State); FAILED(result))
    {
        THROW_HR_WITH_USER_ERROR_MSG(
            result,
            wsl::shared::Localization::MessageWslcContainerStateSaveFailed(wsl::shared::string::MultiByteToWide(ContainerId)),
            "Failed to save the persisted state of container '%hs'",
            ContainerId.c_str());
    }

    return initial;
}

WSLCContainerStateStore::Initial WSLCContainerStateStore::ForExistingContainer(WSLCSessionRuntime& Runtime, const std::string& ContainerId, WSLCContainerStateStorage Storage)
{
    Initial initial{.Runtime = Runtime, .Path = ContainerStatePath(ContainerId)};

    bool missingMeansDefault{};
    switch (Storage)
    {
    case WSLCContainerStateStorage::None:
        missingMeansDefault = true;
        break;

    case WSLCContainerStateStorage::GuestFileV1:
        missingMeansDefault = false;
        break;

    default:
        initial.LoadError = HRESULT_FROM_WIN32(ERROR_UNSUPPORTED_TYPE);
        return initial;
    }

    const auto result = LoadContainerState(initial.Runtime, initial.Path);
    if (result.Status == LoadStatus::Loaded)
    {
        initial.State = result.State;
        return initial;
    }

    if (result.Status != LoadStatus::Missing || !missingMeansDefault)
    {
        initial.LoadError = result.Error;
    }

    return initial;
}

WSLCContainerStateStore::WSLCContainerStateStore(Initial&& InitialState) :
    m_runtime(InitialState.Runtime),
    m_path(std::move(InitialState.Path)),
    m_loadError(InitialState.LoadError),
    m_state(InitialState.State)
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