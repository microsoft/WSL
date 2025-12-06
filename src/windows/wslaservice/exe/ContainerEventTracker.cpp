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

ContainerEventTracker::ContainerTrackingReference::ContainerTrackingReference(ContainerEventTracker::ContainerTrackingReference&& other)
{
    (*this) = std::move(other);
}

ContainerEventTracker::ContainerTrackingReference& ContainerEventTracker::ContainerTrackingReference::operator=(ContainerEventTracker::ContainerTrackingReference&& other)
{
    m_id = other.m_id;
    m_tracker = other.m_tracker;

    other.m_tracker = nullptr;

    return *this;
}

void ContainerEventTracker::ContainerTrackingReference::Reset()
{
    if (m_tracker != nullptr)
    {
        m_tracker->UnregisterContainerStateUpdates(m_tracker != nullptr);
        m_tracker = nullptr;
    }
}

ContainerEventTracker::ContainerTrackingReference::~ContainerTrackingReference()
{
    Reset();
}

ContainerEventTracker::ContainerEventTracker(WSLAVirtualMachine& virtualMachine)
{
    ServiceProcessLauncher launcher{"/usr/bin/nerdctl", {"/usr/bin/nerdctl", "events", "--format", "{{json .}}"}, {}, common::ProcessFlags::Stdout};

    // Redirect stderr to /dev/null to avoid pipe deadlocks.
    launcher.AddFd({.Fd = 2, .Type = WSLAFdTypeLinuxFileOutput, .Path = "/dev/null"});

    auto process = launcher.Launch(virtualMachine);
    m_thread = std::thread(std::bind(&ContainerEventTracker::Run, this, std::move(process)));
}

ContainerEventTracker::~ContainerEventTracker()
{
    // N.B. No callback should be left when the tracker is destroyed.
    WI_ASSERT(m_callbacks.empty());

    m_stopEvent.SetEvent();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
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

    // The 'Event' field is a json string,
    auto innerEventJson = details->get<std::string>();
    auto innerEvent = nlohmann::json::parse(innerEventJson);

    auto containerId = innerEvent.find("container_id");
    THROW_HR_IF_MSG(E_INVALIDARG, containerId == innerEvent.end(), "Failed to parse json: %hs", innerEventJson.c_str());

    WSL_LOG(
        "ContainerStateChange",
        TraceLoggingValue(containerId->get<std::string>().c_str(), "Id"),
        TraceLoggingValue((int)it->second, "State"));

    std::lock_guard lock{m_lock};

    auto containerEntry = m_callbacks.find(containerId->get<std::string>());
    if (containerEntry != m_callbacks.end())
    {
        for (auto& [id, callback] : containerEntry->second)
        {
            callback(it->second);
        }
    }
}

void ContainerEventTracker::Run(ServiceRunningProcess& process)
{
    std::string pendingBuffer;

    wsl::windows::common::relay::MultiHandleWait io;

    auto onStdout = [&](const gsl::span<char>& buffer) {
        // nerdctl events' output is line based. Call OnEvent() for each completed line.

        auto begin = buffer.begin();
        auto end = std::ranges::find(buffer, '\n');
        while (end != buffer.end())
        {
            pendingBuffer.insert(pendingBuffer.end(), begin, end);

            if (!pendingBuffer.empty()) // nerdctl inserts empty lines between events, skip those.
            {
                OnEvent(pendingBuffer);
            }

            pendingBuffer.clear();

            begin = end + 1;
            end = std::ranges::find(begin, buffer.end(), '\n');
        }

        pendingBuffer.insert(pendingBuffer.end(), begin, end);
    };

    auto onStop = [&]() { io.Cancel(); };

    io.AddHandle(std::make_unique<common::relay::ReadHandle>(process.GetStdHandle(1), std::move(onStdout)));
    io.AddHandle(std::make_unique<common::relay::EventHandle>(m_stopEvent.get(), std::move(onStop)));

    if (io.Run({}))
    {
        // TODO: Report error to session.
        WSL_LOG("Unexpected nerdctl exit");
    }
}

ContainerEventTracker::ContainerTrackingReference ContainerEventTracker::RegisterContainerStateUpdates(
    const std::string& ContainerId, ContainerStateChangeCallback&& Callback)
{
    std::lock_guard lock{m_lock};
    auto id = callbackId++;

    m_callbacks[ContainerId][id] = std::move(Callback);
    return ContainerTrackingReference{this, id};
}

void ContainerEventTracker::UnregisterContainerStateUpdates(size_t Id)
{
    std::lock_guard lock{m_lock};

    for (auto& [containerId, callbacks] : m_callbacks)
    {
        auto it = callbacks.find(Id);
        if (it != callbacks.end())
        {
            callbacks.erase(it);
            if (callbacks.empty())
            {
                m_callbacks.erase(containerId);
            }
            return;
        }
    }

    WI_ASSERT(false);
}