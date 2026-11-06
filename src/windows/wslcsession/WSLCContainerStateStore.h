/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerStateStore.h

Abstract:

    Mutable per-container state persisted inside the VM so it survives VM shutdown.

    Each container owns at most one document at /var/lib/docker/containers/<container-id>/wslc-state.json,
    inside the per-container directory the container engine creates and removes along with the container.
    The container metadata label records which storage the container uses (WSLCContainerStateStorage), so
    a missing document can be told apart from a container that never had one.

    Persistence contract:
    - Loading never writes. A document that is unreadable, corrupt, or expected but missing is never
      overwritten; the container runs on in-memory defaults and every flush of a change reports the
      load failure.
    - Mutations only change in-memory state. Flush() writes the latest state and returns the result;
      the caller decides whether a failure is fatal. A failed flush leaves the state pending, so the
      next flush retries it even if no further mutation changed anything.
    - Mutate(), Get() and MarkDeleted() never perform I/O, so they may be called under the container's
      locks. Flush() performs I/O and must not be.
    - MarkDeleted() is final: no flush writes the document after it. Writes never create the document's
      directory, so a flush racing the container's removal cannot recreate it either.

--*/

#pragma once

#include "JsonUtils.h"
#include "WSLCContainerMetadata.h"
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

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

// Access to the files that hold container state. Methods return 0 or an errno, and throw if the files can't be reached.
class IWSLCContainerStateFiles
{
public:
    virtual ~IWSLCContainerStateFiles() = default;

    // Fails with EFBIG if the file is larger than MaxSize.
    virtual int Read(const std::string& Path, size_t MaxSize, std::string& Content) = 0;

    // Atomically replaces the file. Fails with ENOENT instead of creating a missing parent directory.
    virtual int Write(const std::string& Path, std::string_view Content) = 0;
};

std::string ContainerStatePath(const std::string& ContainerId);

WSLCContainerStateLoadResult LoadContainerState(IWSLCContainerStateFiles& Files, const std::string& Path) noexcept;

HRESULT WriteContainerState(IWSLCContainerStateFiles& Files, const std::string& Path, const WSLCPersistedContainerState& State) noexcept;

class WSLCContainerStateStore
{
public:
    NON_COPYABLE(WSLCContainerStateStore);
    NON_MOVABLE(WSLCContainerStateStore);

    // Starting point of a store, resolved before the owning container is constructed.
    struct Initial
    {
        std::shared_ptr<IWSLCContainerStateFiles> Files;
        std::string Path;
        WSLCPersistedContainerState State;
        bool Pending = false;

        // Set when the document could not be loaded. The store then never writes it.
        HRESULT LoadError = S_OK;
    };

    // For a container that was just created: the document is pending.
    static Initial ForNewContainer(std::shared_ptr<IWSLCContainerStateFiles> Files, const std::string& ContainerId);

    // For a container that already exists.
    static Initial ForExistingContainer(std::shared_ptr<IWSLCContainerStateFiles> Files, const std::string& ContainerId, WSLCContainerStateStorage Storage);

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

    // Called once the container is removed. Its document is removed along with it.
    void MarkDeleted() noexcept;

private:
    // Serializes document writes so they land in the order they were requested.
    std::mutex m_ioLock;

    mutable std::mutex m_stateLock;
    const std::shared_ptr<IWSLCContainerStateFiles> m_files;
    const std::string m_path;
    const HRESULT m_loadError;
    _Guarded_by_(m_stateLock) WSLCPersistedContainerState m_state;
    _Guarded_by_(m_stateLock) std::uint64_t m_generation = 0;
    _Guarded_by_(m_stateLock) std::uint64_t m_persistedGeneration = 0;
    _Guarded_by_(m_stateLock) bool m_deleted = false;
};

} // namespace wsl::windows::service::wslc
