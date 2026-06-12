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

DockerEventTracker::DockerEventTracker(DockerHTTPClient& dockerClient, WSLCSession& session, IORelay& relay) : m_session(session)
{
    auto onChunk = [this](const gsl::span<char>& buffer) {
        // docker/podman /events is newline-delimited JSON. A single event can span multiple HTTP
        // chunks - podman flushes large events (e.g. a container with a big label whose create event
        // carries the labels) at ~8KB boundaries - so accumulate chunk bytes and only parse complete,
        // newline-terminated events. Parsing a partial chunk as a whole event would throw and silently
        // drop the event, which previously caused WaitForObjectCreated to time out for large events.
        m_eventBuffer.append(buffer.data(), buffer.size());

        size_t start = 0;
        for (size_t newline; (newline = m_eventBuffer.find('\n', start)) != std::string::npos; start = newline + 1)
        {
            auto event = std::string_view(m_eventBuffer).substr(start, newline - start);
            if (event.empty()) // docker inserts empty lines between events, skip those.
            {
                continue;
            }

            try
            {
                OnEvent(event);
            }
            catch (...)
            {
                WSL_LOG(
                    "DockerEventParseError",
                    TraceLoggingCountedString(
                        event.data(), static_cast<UINT16>(std::min(event.size(), static_cast<size_t>(USHRT_MAX))), "Data"),
                    TraceLoggingValue(wil::ResultFromCaughtException(), "Error"),
                    TraceLoggingValue(m_session.Id(), "SessionId"));
            }
        }

        // Retain any incomplete trailing event for the next chunk.
        m_eventBuffer.erase(0, start);
    };

    auto socket = dockerClient.MonitorEvents();

    relay.AddHandle(std::make_unique<common::io::HTTPChunkBasedReadHandle>(std::move(socket), std::move(onChunk)));
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
        TraceLoggingValue(m_session.Id(), "SessionId"));

    auto parsed = nlohmann::json::parse(event);

    auto action = parsed.find("Action");
    THROW_HR_IF_MSG(E_INVALIDARG, action == parsed.end(), "Failed to parse json: %.*hs", static_cast<int>(event.size()), event.data());

    auto timeEntry = parsed.find("time");
    THROW_HR_IF_MSG(
        E_INVALIDARG, timeEntry == parsed.end(), "Failed to parse time from event: %.*hs", static_cast<int>(event.size()), event.data());
    std::uint64_t eventTime = timeEntry->get<std::uint64_t>();

    auto actionStr = action->get<std::string>();

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

    // Track object creation for WaitForObjectCreated.
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
                m_objectCreated.SetEvent();
            }
            else if (actionStr == "remove")
            {
                std::lock_guard lock{m_lock};
                m_createdObjects.erase(objectId);
            }
        }
    }
}

void DockerEventTracker::OnContainerEvent(const nlohmann::json& parsed, const std::string& action, std::uint64_t eventTime)
{
    static std::map<std::string, ContainerEvent> events{
        {"start", ContainerEvent::Start},
        {"die", ContainerEvent::Stop},
        {"remove", ContainerEvent::Destroy},
        {"exec_die", ContainerEvent::ExecDied},
        {"exec_died", ContainerEvent::ExecDied}};

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
    auto attributes = actor->find("Attributes");
    if (attributes != actor->end())
    {
        auto exitCodeEntry = attributes->find("exitCode");
        if (exitCodeEntry != attributes->end())
        {
            exitCode = std::stoi(exitCodeEntry->get<std::string>());
        }
    }

    std::lock_guard lock{m_lock};

    for (const auto& e : m_containerCallbacks)
    {
        if (e.ContainerId == containerId)
        {
            e.Callback(it->second, exitCode, eventTime);
        }
    }
}

void DockerEventTracker::OnVolumeEvent(const nlohmann::json& parsed, const std::string& action, std::uint64_t eventTime)
{
    static std::map<std::string, VolumeEvent> events{{"create", VolumeEvent::Create}, {"remove", VolumeEvent::Destroy}};

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

    while (true)
    {
        {
            std::lock_guard lock{m_lock};
            if (m_createdObjects.contains(ObjectId))
            {
                return;
            }

            // Reset under the lock so a concurrent OnEvent() that runs after we release the lock
            // and before the wait can re-signal the event and unblock us.
            m_objectCreated.ResetEvent();
        }

        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_TIMEOUT),
            !m_session.WaitForEventOrSessionTerminating(m_objectCreated.get(), c_timeout),
            "Timed out waiting for Docker create event for object '%hs'",
            ObjectId.c_str());
    }
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterContainerStateUpdates(
    const std::string& ContainerId, ContainerStateChangeCallback&& Callback) noexcept
{
    std::lock_guard lock{m_lock};

    auto id = m_callbackId++;
    m_containerCallbacks.emplace_back(id, ContainerId, std::move(Callback));

    return EventTrackingReference{this, id};
}

DockerEventTracker::EventTrackingReference DockerEventTracker::RegisterExecStateUpdates(
    const std::string& ContainerId, ContainerStateChangeCallback&& Callback) noexcept
{
    std::lock_guard lock{m_lock};

    auto id = m_callbackId++;
    m_containerCallbacks.emplace_back(id, ContainerId, std::move(Callback));

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