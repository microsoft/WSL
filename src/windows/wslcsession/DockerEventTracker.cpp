/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DockerEventTracker.cpp

Abstract:

    Contains the implementation of DockerEventTracker.

--*/
#include "precomp.h"
#include "DockerEventTracker.h"
#include "WSLCSession.h"
#include "WSLCVirtualMachine.h"
#include <nlohmann/json.hpp>

using wsl::windows::service::wslc::DockerEventTracker;
using wsl::windows::service::wslc::DockerHTTPClient;
using wsl::windows::service::wslc::WSLCSession;
using wsl::windows::service::wslc::WSLCVirtualMachine;

DockerEventTracker::EventTrackingReference::EventTrackingReference(DockerEventTracker* tracker, size_t id) noexcept :
    m_tracker(tracker), m_id(id)
{
}

DockerEventTracker::EventTrackingReference& DockerEventTracker::EventTrackingReference::operator=(DockerEventTracker::EventTrackingReference&& other) noexcept
{
    Reset();
    m_id = other.m_id;
    m_tracker = other.m_tracker;

    other.m_tracker = nullptr;
    other.m_id = {};

    return *this;
}

void DockerEventTracker::EventTrackingReference::Reset() noexcept
{
    if (m_tracker != nullptr)
    {
        m_tracker->UnregisterCallback(m_id);
        m_tracker = nullptr;
        m_id = {};
    }
}

DockerEventTracker::EventTrackingReference::EventTrackingReference(EventTrackingReference&& other) noexcept :
    m_id(other.m_id), m_tracker(other.m_tracker)
{
    other.m_tracker = nullptr;
    other.m_id = {};
}

DockerEventTracker::EventTrackingReference::~EventTrackingReference() noexcept
{
    Reset();
}

DockerEventTracker::DockerEventTracker(WSLCSession& session) : m_session(session)
{
}

void DockerEventTracker::Connect(DockerHTTPClient& dockerClient, IORelay& relay)
{
    auto onChunk = [this](const gsl::span<char>& buffer) {
        if (!buffer.empty()) // docker inserts empty lines between events, skip those.
        {
            try
            {
                OnEvent(std::string_view(buffer.data(), buffer.size()));
            }
            catch (...)
            {
                WSL_LOG(
                    "DockerEventParseError",
                    TraceLoggingCountedString(
                        buffer.data(), static_cast<UINT16>(std::min(buffer.size(), static_cast<size_t>(USHRT_MAX))), "Data"),
                    TraceLoggingValue(wil::ResultFromCaughtException(), "Error"),
                    TraceLoggingValue(m_session.Id(), "SessionId"));
            }
        }
    };

    auto socket = dockerClient.MonitorEvents();

    relay.AddHandle(std::make_unique<common::io::HTTPChunkBasedReadHandle>(std::move(socket), std::move(onChunk)));
}

DockerEventTracker::~DockerEventTracker()
{
    // N.B. No callback should be left when the tracker is destroyed.
    WI_ASSERT(m_containerCallbacks.empty());
    WI_ASSERT(m_volumeCallbacks.empty());
    WI_ASSERT(m_networkCallbacks.empty());
    WI_ASSERT(m_containerCreateCallbacks.empty());
    WI_ASSERT(m_containerActionCallbacks.empty());
}

void DockerEventTracker::OnEvent(const std::string_view& event)
{
    WSL_LOG(
        "DockerEvent",
        TraceLoggingCountedString(
            event.data(), static_cast<UINT16>(std::min(event.size(), static_cast<size_t>(USHRT_MAX))), "Data"),
        TraceLoggingValue(m_session.Id(), "SessionId"));

    auto parsed = nlohmann::json::parse(event);

    auto action = parsed.find("Action");
    THROW_HR_IF_MSG(E_INVALIDARG, action == parsed.end(), "Failed to parse json: %.*hs", static_cast<int>(event.size()), event.data());

    auto timeNanoEntry = parsed.find("timeNano");
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        timeNanoEntry == parsed.end(),
        "Failed to parse timeNano from event: %.*hs",
        static_cast<int>(event.size()),
        event.data());
    std::int64_t eventTimeNano = timeNanoEntry->get<std::int64_t>();

    auto actionStr = action->get<std::string>();

    // Route events by Type field. Docker uses "container", "volume", "network", etc.
    auto type = parsed.find("Type");
    std::string typeStr = (type != parsed.end()) ? type->get<std::string>() : "container";

    if (typeStr == "container")
    {
        OnContainerEvent(parsed, actionStr, eventTimeNano);

        if (actionStr == "create")
        {
            OnContainerCreated(parsed, eventTimeNano);
        }
    }
    else if (typeStr == "volume")
    {
        OnVolumeEvent(parsed, actionStr, eventTimeNano);
    }
    else if (typeStr == "network")
    {
        OnNetworkEvent(parsed, actionStr, eventTimeNano);
    }
}

void DockerEventTracker::OnContainerEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano)
{
    static std::map<std::string, ContainerEvent> events{
        {"start", ContainerEvent::Start},
        {"die", ContainerEvent::Exit},
        {"destroy", ContainerEvent::Destroy},
        {"exec_die", ContainerEvent::ExecDied},
        {"restart", ContainerEvent::Restart},
        {"health_status: healthy", ContainerEvent::HealthHealthy},
        {"health_status: unhealthy", ContainerEvent::HealthUnhealthy},
        {"kill", ContainerEvent::Kill},
        {"stop", ContainerEvent::Stop}};

    auto actor = parsed.find("Actor");
    THROW_HR_IF_MSG(E_INVALIDARG, actor == parsed.end(), "Missing Actor in container event");

    auto id = actor->find("ID");
    THROW_HR_IF_MSG(E_INVALIDARG, id == actor->end(), "Missing Actor.ID in container event");

    auto containerId = id->get<std::string>();

    std::map<std::string, std::string> attributes;
    if (const auto attributesEntry = actor->find("Attributes"); attributesEntry != actor->end())
    {
        attributes = attributesEntry->get<std::map<std::string, std::string>>();
    }

    auto it = events.find(action);
    if (it == events.end())
    {
        return; // Event is not tracked, dropped.
    }

    std::optional<int> exitCode;
    if (const auto exitCodeEntry = attributes.find("exitCode"); exitCodeEntry != attributes.end())
    {
        exitCode = std::stoi(exitCodeEntry->second);
    }

    std::optional<std::string> execId;
    if (const auto execIdEntry = attributes.find("execID"); execIdEntry != attributes.end())
    {
        execId = execIdEntry->second;
    }

    // Snapshot the matching callbacks so that they can be invoked without holding m_lock. Callbacks can register and
    // unregister callbacks (a container that stops releases its exec processes), which would otherwise mutate the
    // vector being iterated.
    std::vector<std::shared_ptr<ContainerCallback>> callbacks;
    {
        std::lock_guard lock{m_lock};

        for (const auto& e : m_containerCallbacks)
        {
            if (e->ContainerId == containerId && (!e->ExecId.has_value() || e->ExecId == execId))
            {
                callbacks.emplace_back(e);
            }
        }
    }

    const bool handled =
        InvokeCallbacks(callbacks, [&](const ContainerCallback& e) { e.Callback(it->second, exitCode, attributes, eventTimeNano); });

    if (!handled && (it->second == ContainerEvent::Kill || it->second == ContainerEvent::Stop))
    {
        OnContainerAction(containerId, action, attributes, eventTimeNano);
    }
}

void DockerEventTracker::OnContainerAction(
    const std::string& containerId, const std::string& action, const std::map<std::string, std::string>& attributes, std::int64_t eventTimeNano)
{
    std::vector<std::shared_ptr<ContainerActionCallbackEntry>> callbacks;
    {
        std::lock_guard lock{m_lock};
        callbacks = m_containerActionCallbacks;
    }

    InvokeCallbacks(
        callbacks, [&](const ContainerActionCallbackEntry& e) { e.Callback(containerId, action, attributes, eventTimeNano); });
}

void DockerEventTracker::OnVolumeEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano)
{
    static std::map<std::string, VolumeEvent> events{{"create", VolumeEvent::Create}, {"destroy", VolumeEvent::Destroy}};

    auto it = events.find(action);
    if (it == events.end())
    {
        return; // Event is not tracked, dropped.
    }

    auto actor = parsed.find("Actor");
    THROW_HR_IF_MSG(E_INVALIDARG, actor == parsed.end(), "Missing Actor in volume event");

    auto id = actor->find("ID");
    THROW_HR_IF_MSG(E_INVALIDARG, id == actor->end(), "Missing Actor.ID in volume event");

    auto volumeName = id->get<std::string>();

    std::vector<std::shared_ptr<VolumeCallback>> callbacks;
    {
        std::lock_guard lock{m_lock};
        callbacks = m_volumeCallbacks;
    }

    InvokeCallbacks(callbacks, [&](const VolumeCallback& e) { e.Callback(volumeName, it->second, eventTimeNano); });
}

void DockerEventTracker::OnNetworkEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano)
{
    auto actor = parsed.find("Actor");
    THROW_HR_IF_MSG(E_INVALIDARG, actor == parsed.end(), "Missing Actor in network event");

    std::string networkId;
    auto id = actor->find("ID");
    if (id != actor->end())
    {
        networkId = id->get<std::string>();
    }
    else
    {
        // Docker's aggregate prune event reports no network.
        THROW_HR_IF_MSG(E_INVALIDARG, action != "prune", "Missing Actor.ID in network event");
    }

    std::map<std::string, std::string> attributes;
    auto attributesEntry = actor->find("Attributes");
    if (attributesEntry != actor->end())
    {
        attributes = attributesEntry->get<std::map<std::string, std::string>>();
    }

    std::vector<std::shared_ptr<NetworkCallback>> callbacks;
    {
        std::lock_guard lock{m_lock};
        callbacks = m_networkCallbacks;
    }

    InvokeCallbacks(
        callbacks, [&](const NetworkCallback& callback) { callback.Callback(networkId, action, attributes, eventTimeNano); });
}

void DockerEventTracker::OnContainerCreated(const nlohmann::json& parsed, std::int64_t eventTimeNano)
{
    auto actor = parsed.find("Actor");
    THROW_HR_IF_MSG(E_INVALIDARG, actor == parsed.end(), "Missing Actor in container event");

    auto id = actor->find("ID");
    THROW_HR_IF_MSG(E_INVALIDARG, id == actor->end(), "Missing Actor.ID in container event");

    auto containerId = id->get<std::string>();

    std::vector<std::shared_ptr<ContainerCreateCallbackEntry>> callbacks;
    {
        std::lock_guard lock{m_lock};
        callbacks = m_containerCreateCallbacks;
    }

    InvokeCallbacks(callbacks, [&](const ContainerCreateCallbackEntry& e) { e.Callback(containerId, eventTimeNano); });
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterContainerStateUpdates(
    const std::string& ContainerId, ContainerStateChangeCallback&& Callback) noexcept
{
    auto id = m_callbackId++;
    auto entry = std::make_shared<ContainerCallback>(id, std::string{ContainerId}, std::optional<std::string>{}, std::move(Callback));

    std::lock_guard lock{m_lock};
    m_containerCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{this, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterExecStateUpdates(
    const std::string& ContainerId, const std::string& ExecId, ContainerStateChangeCallback&& Callback) noexcept
{
    auto id = m_callbackId++;
    auto entry = std::make_shared<ContainerCallback>(id, std::string{ContainerId}, std::optional<std::string>{ExecId}, std::move(Callback));

    std::lock_guard lock{m_lock};
    m_containerCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{this, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterVolumeUpdates(VolumeEventCallback&& Callback) noexcept
{
    auto id = m_callbackId++;
    auto entry = std::make_shared<VolumeCallback>(id, std::move(Callback));

    std::lock_guard lock{m_lock};
    m_volumeCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{this, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterNetworkUpdates(NetworkEventCallback&& Callback) noexcept
{
    auto id = m_callbackId++;
    auto entry = std::make_shared<NetworkCallback>(id, std::move(Callback));

    std::lock_guard lock{m_lock};
    m_networkCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{this, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterContainerCreate(ContainerCreateCallback&& Callback) noexcept
{
    auto id = m_callbackId++;
    auto entry = std::make_shared<ContainerCreateCallbackEntry>(id, std::move(Callback));

    std::lock_guard lock{m_lock};
    m_containerCreateCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{this, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterContainerActions(ContainerActionCallback&& Callback) noexcept
{
    auto id = m_callbackId++;
    auto entry = std::make_shared<ContainerActionCallbackEntry>(id, std::move(Callback));

    std::lock_guard lock{m_lock};
    m_containerActionCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{this, id};
}

void DockerEventTracker::UnregisterCallback(size_t Id) noexcept
{
    std::shared_ptr<CallbackRegistration> registration;

    {
        std::lock_guard lock{m_lock};

        auto matches = [Id](const auto& e) { return e->CallbackId == Id; };

        auto take = [&](auto& Callbacks) {
            auto entry = std::ranges::find_if(Callbacks, matches);
            if (entry == Callbacks.end())
            {
                return false;
            }

            registration = std::move(*entry);
            Callbacks.erase(entry);
            return true;
        };

        if (!take(m_containerCallbacks) && !take(m_volumeCallbacks) && !take(m_networkCallbacks) &&
            !take(m_containerCreateCallbacks) && !take(m_containerActionCallbacks))
        {
            WI_ASSERT(false);
        }
    }

    if (registration)
    {
        // Wait for any in-flight invocation to complete so the callback can't run once this returns.
        std::lock_guard invokeLock{registration->InvokeLock};
        registration->Unregistered = true;
    }
}
