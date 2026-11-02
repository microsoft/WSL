/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerStateStore.h

Abstract:

    Mutable per-container state persisted in the session storage folder so it survives VM shutdown.

    Each container owns at most one document at <session-storage>/containers/<container-id>.json. The
    container metadata label records which storage the container uses (WSLCContainerStateStorage), so
    a missing document can be told apart from a container that never had one.

    Persistence contract:
    - Loading never writes. A document that is unreadable, corrupt, or expected but missing is never
      overwritten; the container runs on in-memory defaults and every flush of a change reports the
      load failure.
    - Mutations only change in-memory state. Flush() writes the latest state and returns the result;
      the caller decides whether a failure is fatal. A failed flush leaves the state pending, so the
      next flush retries it even if no further mutation changed anything.
    - Mutate() and Get() never perform I/O, so they may be called under the container's locks.
      Flush() and Delete() perform I/O and must not be.
    - Delete() is final: a flush that runs after it, or that was waiting on it, never recreates the
      document.

--*/

#pragma once

#include "JsonUtils.h"
#include "WSLCContainerMetadata.h"
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

namespace wsl::windows::service::wslc {

// Diagnostic state: losing it must never prevent a container from being started, stopped, or removed.
struct WSLCPersistedContainerState
{
    // Number of restarts performed by the container's restart policy since the last explicit start.
    std::uint32_t RestartCount{};

    bool operator==(const WSLCPersistedContainerState&) const = default;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(WSLCPersistedContainerState, RestartCount);

    // Rejects negative, fractional and out of range counts so the document is reported as corrupt instead of narrowed.
    friend void from_json(const nlohmann::json& Json, WSLCPersistedContainerState& State)
    {
        if (!Json.is_object())
        {
            throw nlohmann::json::type_error::create(302, "container state must be an object", &Json);
        }

        State = {};
        const auto restartCount = Json.find("RestartCount");
        if (restartCount == Json.end())
        {
            return;
        }

        if (!restartCount->is_number_unsigned() || restartCount->get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
        {
            throw nlohmann::json::type_error::create(302, "RestartCount must be an unsigned 32-bit integer", &*restartCount);
        }

        State.RestartCount = restartCount->get<std::uint32_t>();
    }
};

enum class WSLCContainerStateLoadStatus
{
    Loaded,
    Missing,
    Corrupt,
    Unreadable,
};

struct WSLCContainerStateLoadResult
{
    WSLCContainerStateLoadStatus Status{};
    WSLCPersistedContainerState State;
    HRESULT Error = S_OK;
};

std::filesystem::path ContainerStatePath(const std::filesystem::path& Directory, const std::string& ContainerId);

WSLCContainerStateLoadResult LoadContainerState(const std::filesystem::path& Path) noexcept;

// Without ReplaceExisting, fails with ERROR_ALREADY_EXISTS instead of overwriting a document.
HRESULT WriteContainerState(const std::filesystem::path& Path, const WSLCPersistedContainerState& State, bool ReplaceExisting) noexcept;

// A missing document is not an error.
HRESULT DeleteContainerState(const std::filesystem::path& Path) noexcept;

// Removes documents and interrupted writes in Directory whose container ID is not in KnownContainerIds.
// Must not run while a container in Directory can be created.
void DeleteOrphanedContainerStates(const std::filesystem::path& Directory, const std::unordered_set<std::string>& KnownContainerIds) noexcept;

class WSLCContainerStateStore
{
public:
    NON_COPYABLE(WSLCContainerStateStore);
    NON_MOVABLE(WSLCContainerStateStore);

    // Starting point of a store, resolved before the owning container is constructed.
    struct Initial
    {
        std::optional<std::filesystem::path> Path;
        WSLCPersistedContainerState State;
        bool DocumentExists = false;
        bool Pending = false;

        // Set when the document could not be loaded. The store then never writes it.
        HRESULT LoadError = S_OK;
    };

    // For a container that was just created: the document is pending and must not already exist.
    // Without a directory, the state is kept in memory only.
    static Initial ForNewContainer(const std::optional<std::filesystem::path>& Directory, const std::string& ContainerId);

    // For a container that already exists.
    static Initial ForExistingContainer(const std::optional<std::filesystem::path>& Directory, const std::string& ContainerId, WSLCContainerStateStorage Storage);

    explicit WSLCContainerStateStore(Initial&& InitialState);

    WSLCPersistedContainerState Get() const;

    template <typename TMutation>
    void Mutate(TMutation&& Mutation)
    {
        std::lock_guard lock{m_stateLock};
        auto state = m_state;
        Mutation(state);
        if (state == m_state)
        {
            return;
        }

        m_state = state;
        m_generation++;
    }

    // Writes the current state if it has changed since the last successful write.
    HRESULT Flush() noexcept;

    HRESULT Delete() noexcept;

private:
    // Serializes document I/O so writes and the delete land in the order they were requested.
    std::mutex m_ioLock;

    mutable std::mutex m_stateLock;
    const std::optional<std::filesystem::path> m_path;
    const HRESULT m_loadError;
    _Guarded_by_(m_stateLock) WSLCPersistedContainerState m_state;
    _Guarded_by_(m_stateLock) std::uint64_t m_generation = 0;
    _Guarded_by_(m_stateLock) std::uint64_t m_persistedGeneration = 0;
    _Guarded_by_(m_stateLock) bool m_documentExists = false;
    _Guarded_by_(m_stateLock) bool m_deleted = false;
};

} // namespace wsl::windows::service::wslc
