/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerEventTracker.cpp

Abstract:

    Contains the implementation of ContainerEventTracker.

--*/
#include "precomp.h"
#include "ContainerEventTracker.h"
#include "WSLCVirtualMachine.h"
#include <nlohmann/json.hpp>

using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::service::wslc::ContainerEventTracker;
using wsl::windows::service::wslc::DockerHTTPClient;
using wsl::windows::service::wslc::WSLCVirtualMachine;

ContainerEventTracker::ContainerTrackingReference::ContainerTrackingReference(ContainerEventTracker* tracker, size_t id) noexcept :
    m_tracker(tracker), m_id(id)
{
}

ContainerEventTracker::ContainerTrackingReference& ContainerEventTracker::ContainerTrackingReference::operator=(
    ContainerEventTracker::ContainerTrackingReference&& other) noexcept
{
    Reset();
    m_id = other.m_id;
    m_tracker = other.m_tracker;

    other.m_tracker = nullptr;
    other.m_id = {};

    return *this;
}

void ContainerEventTracker::ContainerTrackingReference::Reset() noexcept
{
    if (m_tracker != nullptr)
    {
        m_tracker->UnregisterContainerStateUpdates(m_id);
        m_tracker = nullptr;
        m_id = {};
    }
}

ContainerEventTracker::ContainerTrackingReference::ContainerTrackingReference(ContainerTrackingReference&& other) noexcept :
    m_id(other.m_id), m_tracker(other.m_tracker)
{
    other.m_tracker = nullptr;
    other.m_id = {};
}

ContainerEventTracker::ContainerTrackingReference::~ContainerTrackingReference() noexcept
{
    Reset();
}

ContainerEventTracker::ContainerEventTracker(DockerHTTPClient& dockerClient, ULONG sessionId, IORelay& relay) :
    m_sessionId(sessionId)
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

ContainerEventTracker::~ContainerEventTracker()
{
    // N.B. No callback should be left when the tracker is destroyed.
    WI_ASSERT(m_callbacks.empty());
}

void ContainerEventTracker::OnEvent(const std::string_view& event)
{
    WSL_LOG(
        "DockerEvent",
        TraceLoggingCountedString(
            event.data(), static_cast<UINT16>(std::min(event.size(), static_cast<size_t>(USHRT_MAX))), "Data"),
        TraceLoggingValue(m_sessionId, "SessionId"));

    static std::map<std::string, ContainerEvent> events{
        {"start", ContainerEvent::Start}, {"die", ContainerEvent::Stop}, {"exec_die", ContainerEvent::ExecDied}};

    auto parsed = nlohmann::json::parse(event);

    auto action = parsed.find("Action");
    auto actor = parsed.find("Actor");

    THROW_HR_IF_MSG(
        E_INVALIDARG,
        action == parsed.end() || actor == parsed.end(),
        "Failed to parse json: %.*hs",
        static_cast<int>(event.size()),
        event.data());

    auto it = events.find(action->get<std::string>());
    if (it == events.end())
    {
        return; // Event is not tracked, dropped.
    }

    auto id = actor->find("ID");
    THROW_HR_IF_MSG(E_INVALIDARG, id == actor->end(), "Failed to parse json: %.*hs", static_cast<int>(event.size()), event.data());

    auto containerId = id->get<std::string>();

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

    auto timeEntry = parsed.find("time");
    THROW_HR_IF_MSG(
        E_INVALIDARG, timeEntry == parsed.end(), "Failed to parse time from event: %.*hs", static_cast<int>(event.size()), event.data());
    std::uint64_t eventTime = timeEntry->get<std::uint64_t>();

    std::lock_guard lock{m_lock};

    for (const auto& e : m_callbacks)
    {
        if (e.ContainerId == containerId && (!e.ExecId.has_value() || e.ExecId == execId))
        {
            e.Callback(it->second, exitCode, eventTime);
        }
    }
}

ContainerEventTracker::ContainerTrackingReference ContainerEventTracker::RegisterContainerStateUpdates(
    const std::string& ContainerId, ContainerStateChangeCallback&& Callback) noexcept
{
    std::lock_guard lock{m_lock};

    auto id = m_callbackId++;
    m_callbacks.emplace_back(id, ContainerId, std::optional<std::string>{}, std::move(Callback));

    return ContainerTrackingReference{this, id};
}

ContainerEventTracker::ContainerTrackingReference ContainerEventTracker::RegisterExecStateUpdates(
    const std::string& ContainerId, const std::string& ExecId, ContainerStateChangeCallback&& Callback) noexcept
{
    std::lock_guard lock{m_lock};

    auto id = m_callbackId++;
    m_callbacks.emplace_back(id, ContainerId, ExecId, std::move(Callback));

    return ContainerTrackingReference{this, id};
}

void ContainerEventTracker::UnregisterContainerStateUpdates(size_t Id) noexcept
{
    std::lock_guard lock{m_lock};

    auto remove = std::ranges::remove_if(m_callbacks, [Id](auto& entry) { return entry.CallbackId == Id; });
    WI_ASSERT(remove.size() == 1);

    m_callbacks.erase(remove.begin(), remove.end());
}