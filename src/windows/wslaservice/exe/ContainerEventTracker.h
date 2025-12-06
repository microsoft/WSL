#pragma once

#include "ServiceProcessLauncher.h"

namespace wsl::windows::service::wsla {

class WSLAVirtualMachine;

enum class ContainerEvent
{
    Create,
    Start,
    Stop,
    Exit,
    Destroy
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
        ContainerTrackingReference(ContainerTrackingReference&&);
        ~ContainerTrackingReference();

        ContainerTrackingReference& operator=(ContainerTrackingReference&&);

        void Reset();

        size_t m_id;
        ContainerEventTracker* m_tracker = nullptr;
    };

    using ContainerStateChangeCallback = std::function<void(ContainerEvent)>;

    ContainerEventTracker(WSLAVirtualMachine& virtualMachine);
    ~ContainerEventTracker();
    void OnEvent(const std::string& event);

    ContainerTrackingReference RegisterContainerStateUpdates(const std::string& ContainerId, ContainerStateChangeCallback&& Callback);
    void UnregisterContainerStateUpdates(size_t Id);

private:
    void Run(ServiceRunningProcess& process);

    std::map<std::string, std::map<size_t, ContainerStateChangeCallback>> m_callbacks;
    std::thread m_thread;
    wil::unique_event m_stopEvent{wil::EventOptions::ManualReset};
    std::mutex m_lock;
    std::atomic<size_t> callbackId;
};
} // namespace wsl::windows::service::wsla