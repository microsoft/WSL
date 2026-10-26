/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerStateStore.h

Abstract:

    Mutable per-container state persisted in the session storage folder so it survives VM shutdown.
    Each container owns one JSON document at <session-storage>/containers/<container-id>.json that is
    retained for the lifetime of the container.

--*/

#pragma once

#include "JsonUtils.h"
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>

namespace wsl::windows::service::wslc {

struct WSLCContainerStateV1
{
    // Number of restarts performed by the container's restart policy since the last explicit start.
    std::uint32_t RestartCount{};

    bool operator==(const WSLCContainerStateV1&) const = default;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCContainerStateV1, RestartCount);
};

struct WSLCContainerStateDocument
{
    std::optional<WSLCContainerStateV1> V1;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCContainerStateDocument, V1);
};

// Not thread-safe; the owning container serializes access.
class WSLCContainerStateStore
{
public:
    NON_COPYABLE(WSLCContainerStateStore);
    DEFAULT_MOVABLE(WSLCContainerStateStore);

    // Loads the persisted state for the container, creating the document if it does not exist.
    // Without a directory, the state is kept in memory only.
    WSLCContainerStateStore(const std::optional<std::filesystem::path>& Directory, const std::string& ContainerId);

    const WSLCContainerStateV1& Get() const noexcept
    {
        return m_state;
    }

    // Applies Mutate and persists the result if it changed. Persistence failures are logged; the
    // in-memory state is always updated.
    void Update(const std::function<void(WSLCContainerStateV1&)>& Mutate) noexcept;

    void Delete() noexcept;

    const std::optional<std::filesystem::path>& Path() const noexcept
    {
        return m_path;
    }

    static std::filesystem::path PathFor(const std::filesystem::path& Directory, const std::string& ContainerId);

    // Deletes the document of a container that never had a store instance, such as one removed during recovery.
    static void DeleteFor(const std::optional<std::filesystem::path>& Directory, const std::string& ContainerId) noexcept;

    // Removes documents in Directory whose container ID is not in KnownContainerIds.
    static void DeleteOrphans(const std::filesystem::path& Directory, const std::unordered_set<std::string>& KnownContainerIds) noexcept;

private:
    void Save() const;

    std::optional<std::filesystem::path> m_path;
    WSLCContainerStateV1 m_state;
};

} // namespace wsl::windows::service::wslc
