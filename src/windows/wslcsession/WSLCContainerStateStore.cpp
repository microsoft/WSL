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

using wsl::windows::service::wslc::IWSLCContainerStateFiles;
using wsl::windows::service::wslc::WSLCContainerStateLoadResult;
using wsl::windows::service::wslc::WSLCContainerStateLoadStatus;
using wsl::windows::service::wslc::WSLCContainerStateStorage;
using wsl::windows::service::wslc::WSLCContainerStateStore;
using wsl::windows::service::wslc::WSLCPersistedContainerState;

namespace {

constexpr auto c_documentName = "wslc-state.json";
constexpr size_t c_containerIdLength = 64;

// Documents hold a handful of counters, so anything larger is not one of ours.
constexpr size_t c_maxDocumentSize = 64 * 1024;

WSLCContainerStateLoadResult LoadFailure(WSLCContainerStateLoadStatus Status, HRESULT Error) noexcept
{
    return {.Status = Status, .Error = Error};
}

WSLCContainerStateLoadResult Corrupt(const std::string& Path, const char* Reason) noexcept
{
    WSL_LOG("ContainerStateCorrupt", TraceLoggingValue(Path.c_str(), "Path"), TraceLoggingValue(Reason, "Reason"));
    return LoadFailure(WSLCContainerStateLoadStatus::Corrupt, HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT));
}

} // namespace

std::string wsl::windows::service::wslc::ContainerStatePath(const std::string& ContainerId)
{
    // The ID is part of a guest path, so only accept full engine IDs.
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        ContainerId.size() != c_containerIdLength ||
            !std::ranges::all_of(ContainerId, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }),
        "Invalid container ID: '%hs'",
        ContainerId.c_str());

    return std::format("{}/containers/{}/{}", wsl::windows::wslc::ContainerdStorageMountPoint, ContainerId, c_documentName);
}

WSLCContainerStateLoadResult wsl::windows::service::wslc::LoadContainerState(IWSLCContainerStateFiles& Files, const std::string& Path) noexcept
try
{
    std::string content;
    const auto error = Files.Read(Path, c_maxDocumentSize, content);
    switch (error)
    {
    case 0:
        break;
    case ENOENT:
        return LoadFailure(WSLCContainerStateLoadStatus::Missing, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    case EFBIG:
        return Corrupt(Path, "Document too large");
    default:
        WSL_LOG("ContainerStateUnreadable", TraceLoggingValue(Path.c_str(), "Path"), TraceLoggingValue(error, "Errno"));
        return LoadFailure(WSLCContainerStateLoadStatus::Unreadable, E_FAIL);
    }

    auto json = nlohmann::json::parse(content, nullptr, false);
    if (json.is_discarded())
    {
        return Corrupt(Path, "Invalid JSON");
    }

    try
    {
        return {.Status = WSLCContainerStateLoadStatus::Loaded, .State = json.get<WSLCPersistedContainerState>()};
    }
    catch (const nlohmann::json::exception& e)
    {
        return Corrupt(Path, e.what());
    }
}
catch (...)
{
    return LoadFailure(WSLCContainerStateLoadStatus::Unreadable, wil::ResultFromCaughtException());
}

HRESULT wsl::windows::service::wslc::WriteContainerState(IWSLCContainerStateFiles& Files, const std::string& Path, const WSLCPersistedContainerState& State) noexcept
try
{
    const auto error = Files.Write(Path, wsl::shared::ToJson(State));
    RETURN_HR_IF_MSG(E_FAIL, error != 0, "Failed to write '%hs', errno: %d", Path.c_str(), error);

    return S_OK;
}
CATCH_RETURN()

WSLCContainerStateStore::Initial WSLCContainerStateStore::ForNewContainer(std::shared_ptr<IWSLCContainerStateFiles> Files, const std::string& ContainerId)
{
    return {.Files = std::move(Files), .Path = ContainerStatePath(ContainerId), .Pending = true};
}

WSLCContainerStateStore::Initial WSLCContainerStateStore::ForExistingContainer(
    std::shared_ptr<IWSLCContainerStateFiles> Files, const std::string& ContainerId, WSLCContainerStateStorage Storage)
{
    Initial initial{.Files = std::move(Files), .Path = ContainerStatePath(ContainerId)};

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

    const auto result = LoadContainerState(*initial.Files, initial.Path);
    if (result.Status == WSLCContainerStateLoadStatus::Loaded)
    {
        initial.State = result.State;
        return initial;
    }

    if (result.Status != WSLCContainerStateLoadStatus::Missing || !missingMeansDefault)
    {
        initial.LoadError = result.Error;
    }

    return initial;
}

WSLCContainerStateStore::WSLCContainerStateStore(Initial&& InitialState) :
    m_files(std::move(InitialState.Files)),
    m_path(std::move(InitialState.Path)),
    m_loadError(InitialState.LoadError),
    m_state(InitialState.State),
    m_generation(InitialState.Pending ? 1 : 0)
{
    WI_ASSERT(m_files);
}

WSLCPersistedContainerState WSLCContainerStateStore::Get() const
{
    std::lock_guard lock{m_stateLock};
    return m_state;
}

HRESULT WSLCContainerStateStore::Flush() noexcept
try
{
    std::lock_guard ioLock{m_ioLock};

    WSLCPersistedContainerState state;
    std::uint64_t generation{};

    {
        std::lock_guard lock{m_stateLock};
        if (m_deleted || m_generation == m_persistedGeneration)
        {
            return S_OK;
        }

        if (FAILED(m_loadError))
        {
            return m_loadError;
        }

        state = m_state;
        generation = m_generation;
    }

    const auto result = WriteContainerState(*m_files, m_path, state);

    std::lock_guard lock{m_stateLock};

    // The container's removal takes its document with it, so the outcome of a write that raced it is irrelevant.
    if (m_deleted)
    {
        return S_OK;
    }

    RETURN_IF_FAILED(result);

    m_persistedGeneration = generation;
    return S_OK;
}
CATCH_RETURN()

void WSLCContainerStateStore::MarkDeleted() noexcept
{
    std::lock_guard lock{m_stateLock};
    m_deleted = true;
}