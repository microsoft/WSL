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

ContainerEventTracker::ContainerEventTracker(WSLAVirtualMachine& virtualMachine)
{
    ServiceProcessLauncher launcher{nerdctlPath, {nerdctlPath, "events", "--format", "{{json .}}"}, {}, common::ProcessFlags::Stdout};

    // Redirect stderr to /dev/null to avoid pipe deadlocks.
    launcher.AddFd({.Fd = 2, .Type = WSLAFdTypeLinuxFileOutput, .Path = "/dev/null"});

    auto process = launcher.Launch(virtualMachine);
    m_thread = std::thread(std::bind(&ContainerEventTracker::Run, this, std::move(process)));
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
    WSL_LOG("NerdCtlEvent", TraceLoggingValue(event.c_str(), "Data"));

    static std::map<std::string, ContainerEvent> events{
        {"/tasks/create", ContainerEvent::Create},
        {"/tasks/start", ContainerEvent::Start},
        {"/tasks/stop", ContainerEvent::Stop},
        {"/tasks/exit", ContainerEvent::Exit},
        {"/tasks/destroy", ContainerEvent::Destroy}};

    auto parsed = nlohmann::json::parse(event);

    auto type = parsed.find("Topic");
    auto details = parsed.find("Event");

    THROW_HR_IF_MSG(E_INVALIDARG, type == parsed.end() || details == parsed.end(), "Failed to parse json: %hs", event.c_str());

    auto it = events.find(type->get<std::string>());
    if (it == events.end())
    {
        return; // Event is not tracked, dropped.
    }

    // N.B. The 'Event' field is a json string.
    auto innerEventJson = details->get<std::string>();
    auto innerEvent = nlohmann::json::parse(innerEventJson);

    auto containerIdIt = innerEvent.find("container_id");
    THROW_HR_IF_MSG(E_INVALIDARG, containerIdIt == innerEvent.end(), "Failed to parse json: %hs", innerEventJson.c_str());

    std::string containerId = containerIdIt->get<std::string>();

    // Copy callbacks to invoke outside the lock to avoid deadlock if callback tries to register/unregister
    std::vector<ContainerStateChangeCallback> callbacksToInvoke;
    {
        std::lock_guard lock{m_lock};
        for (const auto& e : m_callbacks)
        {
            if (e.ContainerId == containerId)
            {
                callbacksToInvoke.push_back(e.Callback);
            }
        }
    }

    // Invoke callbacks outside the lock
    for (const auto& callback : callbacksToInvoke)
    {
        callback(it->second);
    }
}

void ContainerEventTracker::Run(ServiceRunningProcess& process)
{
    try
    {
        wsl::windows::common::relay::MultiHandleWait io;

        auto oneLineWritten = [&](const gsl::span<char>& buffer) {
            // nerdctl events' output is line based. Call OnEvent() for each completed line.

            if (!buffer.empty()) // nerdctl inserts empty lines between events, skip those.
            {
                OnEvent(std::string{buffer.begin(), buffer.end()});
            }
        };

        auto onStop = [&]() { io.Cancel(); };

        io.AddHandle(std::make_unique<common::relay::LineBasedReadHandle>(process.GetStdHandle(1), std::move(oneLineWritten)));
        io.AddHandle(std::make_unique<common::relay::EventHandle>(m_stopEvent.get(), std::move(onStop)));

        if (io.Run({}))
        {
            // TODO: Report error to session.
            WSL_LOG("Unexpected nerdctl exit");
        }
    }
    CATCH_LOG();
}

ContainerEventTracker::ContainerTrackingReference ContainerEventTracker::RegisterContainerStateUpdates(
    const std::string& ContainerId, ContainerStateChangeCallback&& Callback)
{
    std::lock_guard lock{m_lock};

    auto id = m_callbackId++;
    m_callbacks.emplace_back(id, ContainerId, std::move(Callback));

    return ContainerTrackingReference{this, id};
}

void ContainerEventTracker::UnregisterContainerStateUpdates(size_t Id)
{
    std::lock_guard lock{m_lock};

    auto remove = std::ranges::remove_if(m_callbacks, [Id](auto& entry) { return entry.CallbackId == Id; });
    WI_ASSERT(remove.size() == 1);

    m_callbacks.erase(remove.begin(), remove.end());
}