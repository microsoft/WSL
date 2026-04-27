/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DockerEventTracker.h

Abstract:

    Contains the definition for DockerEventTracker.

--*/

#pragma once

#include "DockerHTTPClient.h"
#include "IORelay.h"

namespace wsl::windows::service::wslc {

class WSLCVirtualMachine;

enum class ContainerEvent
{
    Create,
    Start,
    Stop,
    Exit,
    Destroy,
    ExecDied
};

enum class VolumeEvent
{
    Create,
    Destroy
};

class DockerEventTracker
{
public:
    NON_COPYABLE(DockerEventTracker);
    NON_MOVABLE(DockerEventTracker);

    struct EventTrackingReference
    {
        NON_COPYABLE(EventTrackingReference);

        EventTrackingReference() = default;
        EventTrackingReference(DockerEventTracker* tracker, size_t id) noexcept;
        EventTrackingReference(EventTrackingReference&& other) noexcept;
        ~EventTrackingReference() noexcept;

        EventTrackingReference& operator=(EventTrackingReference&&) noexcept;

        void Reset() noexcept;

        size_t m_id;
        DockerEventTracker* m_tracker = nullptr;
    };

    using ContainerStateChangeCallback = std::function<void(ContainerEvent, std::optional<int>, std::uint64_t)>;
    using VolumeEventCallback = std::function<void(const std::string&, VolumeEvent, std::uint64_t)>;

    DockerEventTracker(DockerHTTPClient& dockerClient, ULONG sessionId, IORelay& relay);
    ~DockerEventTracker();

    EventTrackingReference RegisterContainerStateUpdates(const std::string& ContainerId, ContainerStateChangeCallback&& Callback) noexcept;
    EventTrackingReference RegisterExecStateUpdates(const std::string& ContainerId, const std::string& ExecId, ContainerStateChangeCallback&& Callback) noexcept;
    EventTrackingReference RegisterVolumeUpdates(VolumeEventCallback&& Callback) noexcept;
    void UnregisterCallback(size_t Id) noexcept;

    void WaitForObjectCreated(const std::string& ObjectId);

private:
    void OnEvent(const std::string_view& event);
    void OnContainerEvent(const nlohmann::json& parsed, const std::string& action, std::uint64_t eventTime);
    void OnVolumeEvent(const nlohmann::json& parsed, const std::string& action, std::uint64_t eventTime);

    struct ContainerCallback
    {
        size_t CallbackId;
        std::string ContainerId;
        std::optional<std::string> ExecId;
        ContainerStateChangeCallback Callback;
    };

    struct VolumeCallback
    {
        size_t CallbackId;
        VolumeEventCallback Callback;
    };

    std::vector<ContainerCallback> m_containerCallbacks;
    std::vector<VolumeCallback> m_volumeCallbacks;

    std::unordered_set<std::string> m_createdObjects;
    std::condition_variable_any m_objectStateChanged;

    ULONG m_sessionId{};
    std::recursive_mutex m_lock;
    std::atomic<size_t> m_callbackId{0};
};
} // namespace wsl::windows::service::wslc