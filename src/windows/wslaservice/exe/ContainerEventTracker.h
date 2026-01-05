/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerEventTracker.h

Abstract:

    Contains the definition for ContainerEventTracker.

--*/
#pragma once

#include "DockerHTTPClient.h"

namespace wsl::windows::service::wsla {

class WSLAVirtualMachine;

enum class ContainerEvent
{
    Create,
    Start,
    Stop,
    Exit,
    Destroy,
    ExecDied
};

class ContainerEventTracker
{
public:
    NON_COPYABLE(ContainerEventTracker);
    NON_MOVABLE(ContainerEventTracker);

    struct ContainerTrackingReference
    {
        NON_COPYABLE(ContainerTrackingReference);

        ContainerTrackingReference() = default;
        ContainerTrackingReference(ContainerEventTracker* tracker, size_t id);
        ContainerTrackingReference(ContainerTrackingReference&&) = default;
        ~ContainerTrackingReference();

        ContainerTrackingReference& operator=(ContainerTrackingReference&&);

        void Reset();

        size_t m_id;
        ContainerEventTracker* m_tracker = nullptr;
    };

    using ContainerStateChangeCallback = std::function<void(ContainerEvent, std::optional<int>)>;

    ContainerEventTracker(DockerHTTPClient& dockerClient);
    ~ContainerEventTracker();

    void Stop();

    ContainerTrackingReference RegisterContainerStateUpdates(const std::string& ContainerId, ContainerStateChangeCallback&& Callback);
    ContainerTrackingReference RegisterExecStateUpdates(const std::string& ContainerId, const std::string& ExecId, ContainerStateChangeCallback&& Callback);
    void UnregisterContainerStateUpdates(size_t Id);

private:
    void OnEvent(const std::string& event);
    void Run(wil::unique_socket&& Socket);

    struct Callback
    {
        size_t CallbackId;
        std::string ContainerId;
        std::optional<std::string> ExecId;
        ContainerStateChangeCallback Callback;
    };

    std::vector<Callback> m_callbacks;

    std::thread m_thread;
    wil::unique_event m_stopEvent{wil::EventOptions::ManualReset};
    std::recursive_mutex m_lock;
    std::atomic<size_t> m_callbackId{0};
};
} // namespace wsl::windows::service::wsla