/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DockerEventTracker.cpp

Abstract:

    Contains the implementation of DockerEventTracker.

--*/
#include "precomp.h"
#include "DockerEventTracker.h"
#include <nlohmann/json.hpp>

using wsl::windows::service::wslc::DockerEventTracker;
using wsl::windows::service::wslc::DockerHTTPClient;

namespace {

// Larger than dockerd's event buffer, so every buffered event the stream delivered is still recorded.
constexpr size_t c_deliveryHistorySize = 1024;

} // namespace

DockerEventTracker::EventTrackingReference::EventTrackingReference(std::shared_ptr<RegistryState> state, size_t id) noexcept :
    m_id(id), m_state(std::move(state))
{
}

DockerEventTracker::EventTrackingReference& DockerEventTracker::EventTrackingReference::operator=(DockerEventTracker::EventTrackingReference&& other) noexcept
{
    Reset();
    m_id = other.m_id;
    m_state = std::move(other.m_state);

    other.m_id = {};

    return *this;
}

void DockerEventTracker::EventTrackingReference::Reset() noexcept
{
    if (m_state)
    {
        m_state->UnregisterCallback(m_id);
        m_state.reset();
        m_id = {};
    }
}

DockerEventTracker::EventTrackingReference::EventTrackingReference(EventTrackingReference&& other) noexcept :
    m_id(other.m_id), m_state(std::move(other.m_state))
{
    other.m_id = {};
}

DockerEventTracker::EventTrackingReference::~EventTrackingReference() noexcept
{
    Reset();
}

DockerEventTracker::DockerEventTracker(ULONG SessionId) : m_sessionId(SessionId)
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
                    TraceLoggingValue(m_sessionId, "SessionId"));
            }
        }
    };

    // Subscribe from dockerd's current time so the stream replays anything emitted while the subscription is set up, and
    // WaitForPendingEvents can list exactly the events this stream delivers.
    std::optional<std::int64_t> since;
    try
    {
        since = dockerClient.GetSystemTimeNano();
    }
    CATCH_LOG();

    auto socket = dockerClient.MonitorEvents(since);

    {
        std::lock_guard lock{m_deliveryLock};
        m_streamSince = since;
        m_delivered.clear();
        m_deliveryOrder.clear();
    }

    relay.AddHandle(std::make_unique<common::io::HTTPChunkBasedReadHandle>(std::move(socket), std::move(onChunk)));
}

void DockerEventTracker::WaitForPendingEvents(DockerHTTPClient& dockerClient, std::chrono::milliseconds Timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + Timeout;
    auto remaining = [&]() {
        return std::max(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
            std::chrono::milliseconds{0});
    };

    std::optional<std::int64_t> since;
    {
        std::lock_guard lock{m_deliveryLock};
        since = m_streamSince;
    }

    if (!since.has_value())
    {
        return;
    }

    const auto now = dockerClient.GetSystemTimeNano(remaining());

    // If dockerd's clock moved back past the subscription time, list every buffered event. That includes events emitted
    // before the subscription, which the stream never delivers and so can cost the full timeout.
    const std::int64_t listSince = now < since.value() ? 1 : since.value();

    std::set<EventKey> pending;
    for (const auto& event : dockerClient.ListEvents(listSince, now, remaining()))
    {
        pending.emplace(ParseEventKey(nlohmann::json::parse(event), event));
    }

    std::unique_lock lock{m_deliveryLock};
    const bool delivered = m_deliveryUpdated.wait_until(lock, deadline, [&]() {
        std::erase_if(pending, [&](const EventKey& key) { return m_delivered.contains(key); });
        return pending.empty();
    });

    if (!delivered)
    {
        WSL_LOG(
            "DockerEventsPendingTimeout",
            TraceLoggingValue(pending.size(), "PendingCount"),
            TraceLoggingValue(pending.begin()->TimeNano, "OldestPendingTimeNano"),
            TraceLoggingValue(m_sessionId, "SessionId"));
    }
}

DockerEventTracker::EventKey DockerEventTracker::ParseEventKey(const nlohmann::json& parsed, const std::string_view& event)
{
    auto timeNano = parsed.find("timeNano");
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        timeNano == parsed.end(),
        "Failed to parse timeNano from event: %.*hs",
        static_cast<int>(event.size()),
        event.data());

    return EventKey{.TimeNano = timeNano->get<std::int64_t>(), .ContentHash = std::hash<nlohmann::json>{}(parsed)};
}

bool DockerEventTracker::WasDelivered(const EventKey& key)
{
    std::lock_guard lock{m_deliveryLock};
    return m_delivered.contains(key);
}

void DockerEventTracker::OnEventDelivered(const EventKey& key) noexcept
try
{
    {
        std::lock_guard lock{m_deliveryLock};
        if (m_delivered.insert(key).second)
        {
            m_deliveryOrder.push_back(key);
            if (m_deliveryOrder.size() > c_deliveryHistorySize)
            {
                m_delivered.erase(m_deliveryOrder.front());
                m_deliveryOrder.pop_front();
            }
        }
    }

    m_deliveryUpdated.notify_all();
}
CATCH_LOG();

DockerEventTracker::RegistryState::~RegistryState()
{
    // Tracking references may outlive the tracker, but must unregister before releasing the registry.
    WI_ASSERT(m_containerCallbacks.empty());
    WI_ASSERT(m_volumeCallbacks.empty());
    WI_ASSERT(m_networkCallbacks.empty());
    WI_ASSERT(m_imageCallbacks.empty());
    WI_ASSERT(m_containerCreateCallbacks.empty());
    WI_ASSERT(m_containerActionCallbacks.empty());
}

void DockerEventTracker::OnEvent(const std::string_view& event)
{
    WSL_LOG(
        "DockerEvent",
        TraceLoggingCountedString(
            event.data(), static_cast<UINT16>(std::min(event.size(), static_cast<size_t>(USHRT_MAX))), "Data"),
        TraceLoggingValue(m_sessionId, "SessionId"));

    auto parsed = nlohmann::json::parse(event);

    auto action = parsed.find("Action");
    THROW_HR_IF_MSG(E_INVALIDARG, action == parsed.end(), "Failed to parse json: %.*hs", static_cast<int>(event.size()), event.data());

    const auto key = ParseEventKey(parsed, event);

    // A subscription replays buffered events, so an event emitted while it's set up can also arrive live.
    if (WasDelivered(key))
    {
        WSL_LOG("DockerEventDuplicate", TraceLoggingValue(key.TimeNano, "TimeNano"), TraceLoggingValue(m_sessionId, "SessionId"));
        return;
    }

    // Record the delivery once the callbacks have run, even if one failed, since the event was consumed.
    auto delivered = wil::scope_exit([&]() { OnEventDelivered(key); });

    const auto actionStr = action->get<std::string>();
    const auto eventTimeNano = key.TimeNano;

    // Route events by Type field. Docker uses "container", "volume", "network", "image", etc.
    auto type = parsed.find("Type");
    const std::string typeStr = (type != parsed.end()) ? type->get<std::string>() : "container";

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
    else if (typeStr == "image")
    {
        OnImageEvent(parsed, actionStr, eventTimeNano);
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
        // Docker appends the exec'd command line to exec_create and exec_start.
        if (action.starts_with("exec_create: ") || action.starts_with("exec_start: "))
        {
            OnContainerAction(containerId, action, attributes, eventTimeNano);
        }

        return; // Other events are not tracked, dropped.
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

    // Snapshot the matching callbacks so that they can be invoked without holding the registry lock. Callbacks can register and
    // unregister callbacks (a container that stops releases its exec processes), which would otherwise mutate the
    // vector being iterated.
    std::vector<std::shared_ptr<ContainerCallback>> callbacks;
    {
        std::lock_guard lock{m_state->m_lock};

        for (const auto& e : m_state->m_containerCallbacks)
        {
            if (e->ContainerId == containerId && (!e->ExecId.has_value() || e->ExecId == execId))
            {
                callbacks.emplace_back(e);
            }
        }
    }

    const bool handled =
        InvokeCallbacks(callbacks, [&](const ContainerCallback& e) { e.Callback(it->second, exitCode, attributes, eventTimeNano); });

    // The container state callbacks never publish exec_die, so it's published even when they handled it.
    if (it->second == ContainerEvent::ExecDied || (!handled && (it->second == ContainerEvent::Kill || it->second == ContainerEvent::Stop)))
    {
        OnContainerAction(containerId, action, attributes, eventTimeNano);
    }
}

void DockerEventTracker::OnContainerAction(
    const std::string& containerId, const std::string& action, const std::map<std::string, std::string>& attributes, std::int64_t eventTimeNano)
{
    std::vector<std::shared_ptr<ContainerActionCallbackEntry>> callbacks;
    {
        std::lock_guard lock{m_state->m_lock};
        callbacks = m_state->m_containerActionCallbacks;
    }

    InvokeCallbacks(
        callbacks, [&](const ContainerActionCallbackEntry& e) { e.Callback(containerId, action, attributes, eventTimeNano); });
}

void DockerEventTracker::OnVolumeEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano)
{
    const auto [volumeName, attributes] = ParseResourceActor(parsed, action, "volume");

    std::vector<std::shared_ptr<VolumeCallback>> callbacks;
    {
        std::lock_guard lock{m_state->m_lock};
        callbacks = m_state->m_volumeCallbacks;
    }

    InvokeCallbacks(callbacks, [&](const VolumeCallback& e) { e.Callback(volumeName, action, attributes, eventTimeNano); });
}

void DockerEventTracker::OnNetworkEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano)
{
    const auto [networkId, attributes] = ParseResourceActor(parsed, action, "network");

    std::vector<std::shared_ptr<NetworkCallback>> callbacks;
    {
        std::lock_guard lock{m_state->m_lock};
        callbacks = m_state->m_networkCallbacks;
    }

    InvokeCallbacks(
        callbacks, [&](const NetworkCallback& callback) { callback.Callback(networkId, action, attributes, eventTimeNano); });
}

void DockerEventTracker::OnImageEvent(const nlohmann::json& parsed, const std::string& action, std::int64_t eventTimeNano)
{
    const auto [imageId, attributes] = ParseResourceActor(parsed, action, "image");

    std::vector<std::shared_ptr<ImageCallback>> callbacks;
    {
        std::lock_guard lock{m_state->m_lock};
        callbacks = m_state->m_imageCallbacks;
    }

    InvokeCallbacks(
        callbacks, [&](const ImageCallback& callback) { callback.Callback(imageId, action, attributes, eventTimeNano); });
}

std::pair<std::string, std::map<std::string, std::string>> DockerEventTracker::ParseResourceActor(
    const nlohmann::json& parsed, const std::string& action, const char* type)
{
    auto actor = parsed.find("Actor");
    THROW_HR_IF_MSG(E_INVALIDARG, actor == parsed.end(), "Missing Actor in %hs event", type);

    std::string id;
    if (const auto idEntry = actor->find("ID"); idEntry != actor->end())
    {
        id = idEntry->get<std::string>();
    }
    else
    {
        THROW_HR_IF_MSG(E_INVALIDARG, action != "prune", "Missing Actor.ID in %hs event", type);
    }

    std::map<std::string, std::string> attributes;
    if (const auto attributesEntry = actor->find("Attributes"); attributesEntry != actor->end())
    {
        attributes = attributesEntry->get<std::map<std::string, std::string>>();
    }

    return {std::move(id), std::move(attributes)};
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
        std::lock_guard lock{m_state->m_lock};
        callbacks = m_state->m_containerCreateCallbacks;
    }

    InvokeCallbacks(callbacks, [&](const ContainerCreateCallbackEntry& e) { e.Callback(containerId, eventTimeNano); });
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterContainerStateUpdates(
    const std::string& ContainerId, ContainerStateChangeCallback&& Callback) noexcept
{
    auto id = m_state->m_callbackId++;
    auto entry = std::make_shared<ContainerCallback>(id, std::string{ContainerId}, std::optional<std::string>{}, std::move(Callback));

    std::lock_guard lock{m_state->m_lock};
    m_state->m_containerCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{m_state, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterExecStateUpdates(
    const std::string& ContainerId, const std::string& ExecId, ContainerStateChangeCallback&& Callback) noexcept
{
    auto id = m_state->m_callbackId++;
    auto entry = std::make_shared<ContainerCallback>(id, std::string{ContainerId}, std::optional<std::string>{ExecId}, std::move(Callback));

    std::lock_guard lock{m_state->m_lock};
    m_state->m_containerCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{m_state, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterVolumeUpdates(VolumeEventCallback&& Callback) noexcept
{
    auto id = m_state->m_callbackId++;
    auto entry = std::make_shared<VolumeCallback>(id, std::move(Callback));

    std::lock_guard lock{m_state->m_lock};
    m_state->m_volumeCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{m_state, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterNetworkUpdates(NetworkEventCallback&& Callback) noexcept
{
    auto id = m_state->m_callbackId++;
    auto entry = std::make_shared<NetworkCallback>(id, std::move(Callback));

    std::lock_guard lock{m_state->m_lock};
    m_state->m_networkCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{m_state, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterImageUpdates(ImageEventCallback&& Callback) noexcept
{
    auto id = m_state->m_callbackId++;
    auto entry = std::make_shared<ImageCallback>(id, std::move(Callback));

    std::lock_guard lock{m_state->m_lock};
    m_state->m_imageCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{m_state, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterContainerCreate(ContainerCreateCallback&& Callback) noexcept
{
    auto id = m_state->m_callbackId++;
    auto entry = std::make_shared<ContainerCreateCallbackEntry>(id, std::move(Callback));

    std::lock_guard lock{m_state->m_lock};
    m_state->m_containerCreateCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{m_state, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterContainerActions(ContainerActionCallback&& Callback) noexcept
{
    auto id = m_state->m_callbackId++;
    auto entry = std::make_shared<ContainerActionCallbackEntry>(id, std::move(Callback));

    std::lock_guard lock{m_state->m_lock};
    m_state->m_containerActionCallbacks.emplace_back(std::move(entry));

    return EventTrackingReference{m_state, id};
}

void DockerEventTracker::RegistryState::UnregisterCallback(size_t Id) noexcept
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

        if (!take(m_containerCallbacks) && !take(m_volumeCallbacks) && !take(m_networkCallbacks) && !take(m_imageCallbacks) &&
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
