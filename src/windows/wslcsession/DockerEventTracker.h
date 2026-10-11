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

class WSLCSession;
class WSLCVirtualMachine;

enum class ContainerEvent
{
    Create,
    Start,
    Restart,
    Exit,
    Destroy,
    ExecDied,
    HealthHealthy,
    HealthUnhealthy,
    Kill,
    Stop
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

    using ContainerStateChangeCallback =
        std::function<void(ContainerEvent, std::optional<int>, const std::map<std::string, std::string>&, std::int64_t)>;
    using VolumeEventCallback = std::function<void(const std::string&, VolumeEvent, std::int64_t)>;
    using NetworkEventCallback =
        std::function<void(const std::string&, const std::string&, const std::map<std::string, std::string>&, std::int64_t)>;
    using ContainerCreateCallback = std::function<void(const std::string& ContainerId, std::int64_t TimeNano)>;
    using ContainerActionCallback =
        std::function<void(const std::string& ContainerId, const std::string& Action, const std::map<std::string, std::string>& Attributes, std::int64_t TimeNano)>;

    explicit DockerEventTracker(WSLCSession& session);
    ~DockerEventTracker();

    // Rebinds the tracker on VM start without replacing callback registrations.
    void Connect(DockerHTTPClient& dockerClient, IORelay& relay);

    EventTrackingReference RegisterContainerStateUpdates(const std::string& ContainerId, ContainerStateChangeCallback&& Callback) noexcept;
    EventTrackingReference RegisterExecStateUpdates(const std::string& ContainerId, const std::string& ExecId, ContainerStateChangeCallback&& Callback) noexcept;
    EventTrackingReference RegisterVolumeUpdates(VolumeEventCallback&& Callback) noexcept;
    EventTrackingReference RegisterNetworkUpdates(NetworkEventCallback&& Callback) noexcept;

    // Invoked for every container create event, after the per-container state callbacks. Unlike those,
    // this isn't keyed by container id, because the id isn't known until Docker assigns it.
    EventTrackingReference RegisterContainerCreate(ContainerCreateCallback&& Callback) noexcept;

    // Invoked for 'kill' and 'stop' when no container state callback handled them, such as after the container
    // stopped listening for its events.
    EventTrackingReference RegisterContainerActions(ContainerActionCallback&& Callback) noexcept;
    void UnregisterCallback(size_t Id) noexcept;

private:
    void OnEvent(const std::string_view& event);
    void OnContainerEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano);
    void OnContainerAction(const std::string& containerId, const std::string& action, const std::map<std::string, std::string>& attributes, std::int64_t eventTimeNano);
    void OnContainerCreated(const nlohmann::json& parsed, std::int64_t eventTimeNano);
    void OnVolumeEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano);
    void OnNetworkEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano);

    // Callbacks are invoked without holding m_lock so that a callback can register or unregister callbacks, and so
    // that a callback taking its own lock can't invert with a thread that registers a callback under that same lock.
    struct CallbackRegistration
    {
        NON_COPYABLE(CallbackRegistration);
        NON_MOVABLE(CallbackRegistration);

        CallbackRegistration(size_t Id) noexcept : CallbackId(Id)
        {
        }

        const size_t CallbackId;

        // Held while the callback runs so it can't be invoked once UnregisterCallback() returned for it.
        // N.B. Recursive so a running callback can unregister itself.
        std::recursive_mutex InvokeLock;
        _Guarded_by_(InvokeLock) bool Unregistered = false;
    };

    struct ContainerCallback : CallbackRegistration
    {
        ContainerCallback(size_t Id, std::string&& ContainerId, std::optional<std::string>&& ExecId, ContainerStateChangeCallback&& Callback) :
            CallbackRegistration(Id), ContainerId(std::move(ContainerId)), ExecId(std::move(ExecId)), Callback(std::move(Callback))
        {
        }

        const std::string ContainerId;
        const std::optional<std::string> ExecId;
        const ContainerStateChangeCallback Callback;
    };

    struct VolumeCallback : CallbackRegistration
    {
        VolumeCallback(size_t Id, VolumeEventCallback&& Callback) : CallbackRegistration(Id), Callback(std::move(Callback))
        {
        }

        const VolumeEventCallback Callback;
    };

    struct NetworkCallback : CallbackRegistration
    {
        NetworkCallback(size_t Id, NetworkEventCallback&& Callback) : CallbackRegistration(Id), Callback(std::move(Callback))
        {
        }

        const NetworkEventCallback Callback;
    };

    struct ContainerCreateCallbackEntry : CallbackRegistration
    {
        ContainerCreateCallbackEntry(size_t Id, ContainerCreateCallback&& Callback) :
            CallbackRegistration(Id), Callback(std::move(Callback))
        {
        }

        const ContainerCreateCallback Callback;
    };

    struct ContainerActionCallbackEntry : CallbackRegistration
    {
        ContainerActionCallbackEntry(size_t Id, ContainerActionCallback&& Callback) :
            CallbackRegistration(Id), Callback(std::move(Callback))
        {
        }

        const ContainerActionCallback Callback;
    };

    _Guarded_by_(m_lock) std::vector<std::shared_ptr<ContainerCallback>> m_containerCallbacks;
    _Guarded_by_(m_lock) std::vector<std::shared_ptr<VolumeCallback>> m_volumeCallbacks;
    _Guarded_by_(m_lock) std::vector<std::shared_ptr<NetworkCallback>> m_networkCallbacks;
    _Guarded_by_(m_lock) std::vector<std::shared_ptr<ContainerCreateCallbackEntry>> m_containerCreateCallbacks;
    _Guarded_by_(m_lock) std::vector<std::shared_ptr<ContainerActionCallbackEntry>> m_containerActionCallbacks;

    // Invokes a snapshot of callbacks taken under m_lock, skipping registrations that have since been unregistered.
    // Returns whether any callback ran.
    template <typename TCallback, typename TInvoke>
    static bool InvokeCallbacks(const std::vector<std::shared_ptr<TCallback>>& Callbacks, const TInvoke& Invoke)
    {
        bool invoked = false;
        for (const auto& e : Callbacks)
        {
            std::lock_guard invokeLock{e->InvokeLock};
            if (!e->Unregistered)
            {
                Invoke(*e);
                invoked = true;
            }
        }

        return invoked;
    }

    WSLCSession& m_session;
    std::mutex m_lock;
    std::atomic<size_t> m_callbackId{0};
};
} // namespace wsl::windows::service::wslc
