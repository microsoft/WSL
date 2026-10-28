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
    struct CallbackRegistry;

public:
    NON_COPYABLE(DockerEventTracker);
    NON_MOVABLE(DockerEventTracker);

    struct EventTrackingReference
    {
        NON_COPYABLE(EventTrackingReference);

        EventTrackingReference() = default;
        EventTrackingReference(std::shared_ptr<CallbackRegistry> registry, size_t id) noexcept;
        EventTrackingReference(EventTrackingReference&& other) noexcept;
        ~EventTrackingReference() noexcept;

        EventTrackingReference& operator=(EventTrackingReference&&) noexcept;

        void Reset() noexcept;

    private:
        size_t m_id{};
        std::shared_ptr<CallbackRegistry> m_registry;
    };

    using ContainerStateChangeCallback =
        std::function<void(ContainerEvent, std::optional<int>, const std::map<std::string, std::string>&, std::int64_t)>;
    using VolumeEventCallback =
        std::function<void(const std::string&, const std::string&, const std::map<std::string, std::string>&, std::int64_t)>;
    using NetworkEventCallback =
        std::function<void(const std::string&, const std::string&, const std::map<std::string, std::string>&, std::int64_t)>;
    using ContainerCreateCallback = std::function<void(const std::string& ContainerId, std::int64_t TimeNano)>;
    using ContainerActionCallback =
        std::function<void(const std::string& ContainerId, const std::string& Action, const std::map<std::string, std::string>& Attributes, std::int64_t TimeNano)>;

    explicit DockerEventTracker(ULONG SessionId);

    // Rebinds the tracker on VM start without replacing callback registrations.
    void Connect(DockerHTTPClient& dockerClient, IORelay& relay);

    void OnEvent(const std::string_view& event);

    EventTrackingReference RegisterContainerStateUpdates(const std::string& ContainerId, ContainerStateChangeCallback&& Callback) noexcept;
    EventTrackingReference RegisterExecStateUpdates(const std::string& ContainerId, const std::string& ExecId, ContainerStateChangeCallback&& Callback) noexcept;
    EventTrackingReference RegisterVolumeUpdates(VolumeEventCallback&& Callback) noexcept;
    EventTrackingReference RegisterNetworkUpdates(NetworkEventCallback&& Callback) noexcept;

    // Invoked for every container create event, after the per-container state callbacks. Unlike those,
    // this isn't keyed by container id, because the id isn't known until Docker assigns it.
    EventTrackingReference RegisterContainerCreate(ContainerCreateCallback&& Callback) noexcept;

    // Invoked for 'kill' and 'stop' when no container state callback handled them, such as after the container
    // stopped listening for its events, and for every 'exec_create', 'exec_start' and 'exec_die'.
    // Runs after the container state callbacks.
    EventTrackingReference RegisterContainerActions(ContainerActionCallback&& Callback) noexcept;

private:
    void OnContainerEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano);
    void OnContainerAction(const std::string& containerId, const std::string& action, const std::map<std::string, std::string>& attributes, std::int64_t eventTimeNano);
    void OnContainerCreated(const nlohmann::json& parsed, std::int64_t eventTimeNano);
    void OnVolumeEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano);
    void OnNetworkEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano);

    // Volume and network events identify their resource by Actor.ID, except Docker's aggregate prune event, which reports none.
    static std::pair<std::string, std::map<std::string, std::string>> ParseResourceActor(
        const nlohmann::json& parsed, const std::string& action, const char* type);

    // Invoke outside the registry lock so callbacks can change registrations or acquire their own locks.
    struct CallbackRegistration
    {
        NON_COPYABLE(CallbackRegistration);
        NON_MOVABLE(CallbackRegistration);

        CallbackRegistration(size_t Id) noexcept : CallbackId(Id)
        {
        }

        const size_t CallbackId;

        // Drain callbacks during unregister; recursion allows a callback to unregister itself.
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

    // Tokens can outlive the session, so their registry must not retain the tracker or session.
    struct CallbackRegistry
    {
        ~CallbackRegistry()
        {
            WI_ASSERT(ContainerCallbacks.empty());
            WI_ASSERT(VolumeCallbacks.empty());
            WI_ASSERT(NetworkCallbacks.empty());
            WI_ASSERT(ContainerCreateCallbacks.empty());
            WI_ASSERT(ContainerActionCallbacks.empty());
        }

        void UnregisterCallback(size_t Id) noexcept;

        std::mutex Lock;
        std::atomic<size_t> CallbackId{0};
        _Guarded_by_(Lock) std::vector<std::shared_ptr<ContainerCallback>> ContainerCallbacks;
        _Guarded_by_(Lock) std::vector<std::shared_ptr<VolumeCallback>> VolumeCallbacks;
        _Guarded_by_(Lock) std::vector<std::shared_ptr<NetworkCallback>> NetworkCallbacks;
        _Guarded_by_(Lock) std::vector<std::shared_ptr<ContainerCreateCallbackEntry>> ContainerCreateCallbacks;
        _Guarded_by_(Lock) std::vector<std::shared_ptr<ContainerActionCallbackEntry>> ContainerActionCallbacks;
    };

    // Invoke a snapshot, skipping unregistered entries, and report whether any callback ran.
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

    const ULONG m_sessionId;
    const std::shared_ptr<CallbackRegistry> m_registry = std::make_shared<CallbackRegistry>();
};
} // namespace wsl::windows::service::wslc
