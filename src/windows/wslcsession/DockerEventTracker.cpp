/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DockerEventTracker.cpp

Abstract:

    Contains the implementation of DockerEventTracker.

--*/
#include "precomp.h"
#include "DockerEventTracker.h"
#include "WSLCVirtualMachine.h"
#include <nlohmann/json.hpp>

using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::service::wslc::DockerEventTracker;
using wsl::windows::service::wslc::DockerHTTPClient;
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

DockerEventTracker::DockerEventTracker(DockerHTTPClient& dockerClient, ULONG sessionId, IORelay& relay) : m_sessionId(sessionId)
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
                    TraceLoggingValue(buffer.data(), "Data"),
                    TraceLoggingValue(wil::ResultFromCaughtException(), "Error"),
                    TraceLoggingValue(m_sessionId, "SessionId"));
            }
        }
    };

    auto socket = dockerClient.MonitorEvents();

    relay.AddHandle(std::make_unique<common::relay::HTTPChunkBasedReadHandle>(std::move(socket), std::move(onChunk)));
}

DockerEventTracker::~DockerEventTracker()
{
    // N.B. No callback should be left when the tracker is destroyed.
    WI_ASSERT(m_containerCallbacks.empty());
    WI_ASSERT(m_volumeCallbacks.empty());
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

    auto timeEntry = parsed.find("time");
    THROW_HR_IF_MSG(
        E_INVALIDARG, timeEntry == parsed.end(), "Failed to parse time from event: %.*hs", static_cast<int>(event.size()), event.data());
    std::uint64_t eventTime = timeEntry->get<std::uint64_t>();

    auto actionStr = action->get<std::string>();

    // Track object lifecycle for WaitForObjectCreated/WaitForObjectDestroyed.
    auto actor = parsed.find("Actor");
    if (actor != parsed.end())
    {
        auto id = actor->find("ID");
        if (id != actor->end())
        {
            auto objectId = id->get<std::string>();
            if (actionStr == "create")
            {
                std::lock_guard lock{m_lock};
                m_createdObjects.insert(objectId);
                m_objectStateChanged.notify_all();
            }
            else if (actionStr == "destroy")
            {
                std::lock_guard lock{m_lock};
                m_createdObjects.erase(objectId);
                m_objectStateChanged.notify_all();
            }
        }
    }

    // Route events by Type field. Docker uses "container", "volume", "network", etc.
    auto type = parsed.find("Type");
    std::string typeStr = (type != parsed.end()) ? type->get<std::string>() : "container";

    if (typeStr == "container")
    {
        OnContainerEvent(parsed, actionStr, eventTime);
    }
    else if (typeStr == "volume")
    {
        OnVolumeEvent(parsed, actionStr, eventTime);
    }
}

void DockerEventTracker::OnContainerEvent(const nlohmann::json& parsed, const std::string& action, std::uint64_t eventTime)
{
    static std::map<std::string, ContainerEvent> events{
        {"start", ContainerEvent::Start}, {"die", ContainerEvent::Stop}, {"exec_die", ContainerEvent::ExecDied}};

    auto actor = parsed.find("Actor");
    THROW_HR_IF_MSG(E_INVALIDARG, actor == parsed.end(), "Missing Actor in container event");

    auto id = actor->find("ID");
    THROW_HR_IF_MSG(E_INVALIDARG, id == actor->end(), "Missing Actor.ID in container event");

    auto containerId = id->get<std::string>();

    auto it = events.find(action);
    if (it == events.end())
    {
        return; // Event is not tracked, dropped.
    }

    std::optional<int> exitCode;
    std::optional<std::string> execId;
    auto attributes = actor->find("Attributes");
    if (attributes != actor->end())
    {
        auto exitCodeEntry = attributes->find("exitCode");
        if (exitCodeEntry != attributes->end())
        {
            exitCode = std::stoi(exitCodeEntry->get<std::string>());
        }

        auto execIdEntry = attributes->find("execID");
        if (execIdEntry != attributes->end())
        {
            execId = execIdEntry->get<std::string>();
        }
    }

    std::lock_guard lock{m_lock};

    for (const auto& e : m_containerCallbacks)
    {
        if (e.ContainerId == containerId && (!e.ExecId.has_value() || e.ExecId == execId))
        {
            e.Callback(it->second, exitCode, eventTime);
        }
    }
}

void DockerEventTracker::OnVolumeEvent(const nlohmann::json& parsed, const std::string& action, std::uint64_t eventTime)
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

    std::lock_guard lock{m_lock};

    for (const auto& e : m_volumeCallbacks)
    {
        e.Callback(volumeName, it->second, eventTime);
    }
}

void DockerEventTracker::WaitForObjectCreated(const std::string& ObjectId)
{
    constexpr auto c_timeout = std::chrono::seconds{60};

    std::unique_lock lock{m_lock};
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_TIMEOUT),
        !m_objectStateChanged.wait_for(lock, c_timeout, [&]() { return m_createdObjects.contains(ObjectId); }),
        "Timed out waiting for Docker create event for object '%hs'",
        ObjectId.c_str());
}

void DockerEventTracker::WaitForObjectDestroyed(const std::string& ObjectId)
{
    constexpr auto c_timeout = std::chrono::seconds{60};

    std::unique_lock lock{m_lock};
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_TIMEOUT),
        !m_objectStateChanged.wait_for(lock, c_timeout, [&]() { return !m_createdObjects.contains(ObjectId); }),
        "Timed out waiting for Docker destroy event for object '%hs'",
        ObjectId.c_str());
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterContainerStateUpdates(
    const std::string& ContainerId, ContainerStateChangeCallback&& Callback) noexcept
{
    std::lock_guard lock{m_lock};

    auto id = m_callbackId++;
    m_containerCallbacks.emplace_back(id, ContainerId, std::optional<std::string>{}, std::move(Callback));

    return EventTrackingReference{this, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterExecStateUpdates(
    const std::string& ContainerId, const std::string& ExecId, ContainerStateChangeCallback&& Callback) noexcept
{
    std::lock_guard lock{m_lock};

    auto id = m_callbackId++;
    m_containerCallbacks.emplace_back(id, ContainerId, ExecId, std::move(Callback));

    return EventTrackingReference{this, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterVolumeUpdates(VolumeEventCallback&& Callback) noexcept
{
    std::lock_guard lock{m_lock};

    auto id = m_callbackId++;
    m_volumeCallbacks.emplace_back(id, std::move(Callback));

    return EventTrackingReference{this, id};
}

void DockerEventTracker::UnregisterCallback(size_t Id) noexcept
{
    std::lock_guard lock{m_lock};

    // Try container callbacks first.
    auto containerRemove = std::ranges::remove_if(m_containerCallbacks, [Id](auto& entry) { return entry.CallbackId == Id; });
    if (!containerRemove.empty())
    {
        WI_ASSERT(containerRemove.size() == 1);
        m_containerCallbacks.erase(containerRemove.begin(), containerRemove.end());
        return;
    }

    // Then volume callbacks.
    auto volumeRemove = std::ranges::remove_if(m_volumeCallbacks, [Id](auto& entry) { return entry.CallbackId == Id; });
    WI_ASSERT(volumeRemove.size() == 1);
    m_volumeCallbacks.erase(volumeRemove.begin(), volumeRemove.end());
}