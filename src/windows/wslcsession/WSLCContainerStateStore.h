/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerStateStore.h

Abstract:

    Mutable per-container state persisted inside the VM so it survives VM shutdown.

    Each container owns at most one document at
    /var/lib/docker/containers/<container-id>/wslc-container-state.json, inside the per-container
    directory the container engine creates and removes along with the container.
    The container metadata label records which storage the container uses (WSLCContainerStateStorage), so
    a missing document can be told apart from a container that never had one.

    Scope:
    - Each document belongs to exactly one container and holds only that container's independent state.
    - The store provides no consistency, ordering, or transaction guarantees across containers.
    - It must not be used for state that spans or coordinates multiple containers, such as project-wide or
      session-wide state. Such features need a separate storage design.

    Persistence contract:
    - A new container's document is written before the container is returned; failing to write it fails
      the creation.
    - Loading never writes. A document that is unreadable, corrupt, or expected but missing is never
      overwritten; the container runs on in-memory defaults and every update that changes the state
      reports the load failure.
    - Update() writes the state when it changes and returns the result; the caller decides whether a
      failure is fatal. A failed write is retried by the next update even if that update changes nothing.
    - Writes never create the document's directory, so a write racing the container's removal cannot
      recreate it.

--*/

#pragma once

#include "JsonUtils.h"
#include "WSLCContainerMetadata.h"
#include <mutex>
#include <string>

namespace wsl::windows::service::wslc {

class WSLCSessionRuntime;

// Diagnostic state: losing it must never prevent a container from being started, stopped, or removed.
struct WSLCPersistedContainerState
{
    // Number of restarts performed by the container's restart policy since the last explicit start.
    std::uint32_t RestartCount{};

    bool operator==(const WSLCPersistedContainerState&) const = default;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCPersistedContainerState, RestartCount);
};

class WSLCContainerStateStore
{
public:
    NON_COPYABLE(WSLCContainerStateStore);
    NON_MOVABLE(WSLCContainerStateStore);

    // Writes the initial document of a container that was just created, and returns its state.
    static WSLCPersistedContainerState ForNewContainer(WSLCSessionRuntime& Runtime, const std::string& ContainerId);

    // Loads the state of a container that already exists. Throws if the document can't be loaded.
    static WSLCPersistedContainerState ForExistingContainer(WSLCSessionRuntime& Runtime, const std::string& ContainerId, WSLCContainerStateStorage Storage);

    // LoadError is set when the document could not be loaded. The store then never writes it.
    WSLCContainerStateStore(WSLCSessionRuntime& Runtime, const std::string& ContainerId, const WSLCPersistedContainerState& State, HRESULT LoadError);

    WSLCPersistedContainerState Get() const;

    // Applies Mutation and writes the document if the state changed or the last write failed.
    template <typename TMutation>
    HRESULT Update(TMutation&& Mutation)
    {
        std::lock_guard lock{m_lock};
        auto state = m_state;
        Mutation(state);
        if (state == m_state && !m_writeFailed)
        {
            return S_OK;
        }

        m_state = state;
        return WriteLockHeld();
    }

private:
    _Requires_lock_held_(m_lock)
    HRESULT WriteLockHeld() noexcept;

    // Held across writes so they land in the order they were requested.
    mutable std::mutex m_lock;
    WSLCSessionRuntime& m_runtime;
    const std::string m_path;
    const HRESULT m_loadError;
    _Guarded_by_(m_lock) WSLCPersistedContainerState m_state;
    _Guarded_by_(m_lock) bool m_writeFailed = false;
};

} // namespace wsl::windows::service::wslc
