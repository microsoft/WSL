/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerEventTracker.cpp

Abstract:

    Contains the implementation of ContainerEventTracker.

--*/
#include "precomp.h"
#include "ContainerEventTracker.h"
#include "WSLAVirtualMachine.h"
#include <nlohmann/json.hpp>

using wsl::windows::service::wsla::ContainerEventTracker;
using wsl::windows::service::wsla::DockerHTTPClient;
using wsl::windows::service::wsla::WSLAVirtualMachine;

ContainerEventTracker::ContainerTrackingReference::ContainerTrackingReference(ContainerEventTracker* tracker, size_t id) :
    m_tracker(tracker), m_id(id)
{
}

ContainerEventTracker::ContainerTrackingReference& ContainerEventTracker::ContainerTrackingReference::operator=(ContainerEventTracker::ContainerTrackingReference&& other)
{
    Reset();
    m_id = other.m_id;
    m_tracker = other.m_tracker;

    other.m_tracker = nullptr;
    other.m_id = {};

    return *this;
}

void ContainerEventTracker::ContainerTrackingReference::Reset()
{
    if (m_tracker != nullptr)
    {
        m_tracker->UnregisterContainerStateUpdates(m_id);
        m_tracker = nullptr;
        m_id = {};
    }
}

ContainerEventTracker::ContainerTrackingReference::~ContainerTrackingReference()
{
    Reset();
}

ContainerEventTracker::ContainerEventTracker(DockerHTTPClient& dockerClient)
{
    auto socket = dockerClient.MonitorEvents();
    m_thread = std::thread([socket = std::move(socket), this]() mutable { Run(std::move(socket)); }

    );
}

void ContainerEventTracker::Stop()
{
    // N.B. No callback should be left when the tracker is destroyed.
    m_stopEvent.SetEvent();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

ContainerEventTracker::~ContainerEventTracker()
{
    // N.B. No callback should be left when the tracker is destroyed.
    WI_ASSERT(m_callbacks.empty());

    Stop();
}

void ContainerEventTracker::OnEvent(const std::string& event)
{
    // TODO: log session ID
    WSL_LOG("DockerEvent", TraceLoggingValue(event.c_str(), "Data"));

    static std::map<std::string, ContainerEvent> events{
        {"start", ContainerEvent::Start}, {"die", ContainerEvent::Stop}, {"exec_die", ContainerEvent::ExecDied}};

    auto parsed = nlohmann::json::parse(event);

    auto action = parsed.find("Action");
    auto actor = parsed.find("Actor");

    THROW_HR_IF_MSG(E_INVALIDARG, action == parsed.end() || actor == parsed.end(), "Failed to parse json: %hs", event.c_str());

    auto it = events.find(action->get<std::string>());
    if (it == events.end())
    {
        return; // Event is not tracked, dropped.
    }

    auto id = actor->find("ID");
    THROW_HR_IF_MSG(E_INVALIDARG, id == actor->end(), "Failed to parse json: %hs", event.c_str());

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

    std::lock_guard lock{m_lock};

    for (const auto& e : m_callbacks)
    {
        if (e.ContainerId == containerId && (!e.ExecId.has_value() || e.ExecId == execId))
        {
            e.Callback(it->second, exitCode);
        }
    }
}

void ContainerEventTracker::Run(wil::unique_socket&& socket)
try
{
    wsl::windows::common::relay::MultiHandleWait io;

    auto oneLineWritten = [&](const gsl::span<char>& buffer) {
        // docker events' output is line based. Call OnEvent() for each completed line.

        if (!buffer.empty()) // docker inserts empty lines between events, skip those.
        {
            try
            {
                OnEvent(std::string{buffer.begin(), buffer.end()});
            }
            catch (...)
            {
                WSL_LOG(
                    "DockerEventParseError",
                    TraceLoggingValue(buffer.data(), "Data"),
                    TraceLoggingValue(wil::ResultFromCaughtException(), "Error"));
            }
        }
    };

    auto onStop = [&]() { io.Cancel(); };

    io.AddHandle(std::make_unique<common::relay::HTTPChunkBasedReadHandle>(wil::unique_handle{(HANDLE)socket.release()}, std::move(oneLineWritten)));
    io.AddHandle(std::make_unique<common::relay::EventHandle>(m_stopEvent.get(), std::move(onStop)));

    if (io.Run({}))
    {
        // TODO: Report error to session.
        WSL_LOG("Unexpected docker exit");
    }
}
CATCH_LOG();

ContainerEventTracker::ContainerTrackingReference ContainerEventTracker::RegisterContainerStateUpdates(
    const std::string& ContainerId, ContainerStateChangeCallback&& Callback)
{
    std::lock_guard lock{m_lock};

    auto id = m_callbackId++;
    m_callbacks.emplace_back(id, ContainerId, std::optional<std::string>{}, std::move(Callback));

    return ContainerTrackingReference{this, id};
}

ContainerEventTracker::ContainerTrackingReference ContainerEventTracker::RegisterExecStateUpdates(
    const std::string& ContainerId, const std::string& ExecId, ContainerStateChangeCallback&& Callback)
{
    std::lock_guard lock{m_lock};

    auto id = m_callbackId++;
    m_callbacks.emplace_back(id, ContainerId, ExecId, std::move(Callback));

    return ContainerTrackingReference{this, id};
}

void ContainerEventTracker::UnregisterContainerStateUpdates(size_t Id)
{
    std::lock_guard lock{m_lock};

    auto remove = std::ranges::remove_if(m_callbacks, [Id](auto& entry) { return entry.CallbackId == Id; });
    WI_ASSERT(remove.size() == 1);

    m_callbacks.erase(remove.begin(), remove.end());
}