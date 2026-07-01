// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "EventStore.h"
#include "WSLCSession.h"
#include "WSLCExecutionContext.h"
#include <chrono>
#include "wslc_schema.h"

using wsl::shared::Localization;

namespace wsl::windows::service::wslc {

namespace {

    // Normalizes a seconds-since-epoch window bound from the COM boundary, treating zero as "unset".
    std::optional<uint64_t> ToTimeBound(uint64_t TimeSeconds)
    {
        if (TimeSeconds == 0)
        {
            return std::nullopt;
        }

        return TimeSeconds;
    }

} // namespace

void EventStore::Append(wsl::windows::common::wslc_schema::Event Event)
{
    std::lock_guard lock(m_lock);

    m_events.push_back(std::move(Event));

    if (m_events.size() > c_eventRingCapacity)
    {
        m_events.pop_front();
        ++m_firstSequenceNumber;
    }

    m_updated.notify_all();
}

void EventStore::Record(std::string Type, std::string Action, const std::string& ActorId, std::optional<uint64_t> TimeSeconds) noexcept
try
{
    wsl::windows::common::wslc_schema::Event event;
    event.Type = std::move(Type);
    event.Action = std::move(Action);
    event.Actor.ID = ActorId;

    event.time = TimeSeconds.value_or(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));

    Append(std::move(event));
}
CATCH_LOG()

namespace {

    // Values sharing a key are OR'd, distinct keys are AND'd. Unrecognized keys are ignored.
    bool EventMatchesFilters(const wsl::windows::common::wslc_schema::Event& event, const std::map<std::string, std::vector<std::string>>& filters)
    {
        for (const auto& [key, values] : filters)
        {
            if (key == "type")
            {
                if (!std::ranges::any_of(values, [&](const std::string& v) { return event.Type == v; }))
                {
                    return false;
                }
            }
            else if (key == "event")
            {
                if (!std::ranges::any_of(values, [&](const std::string& v) { return event.Action == v; }))
                {
                    return false;
                }
            }
            else if (key == "container")
            {
                if (event.Type != "container" ||
                    !std::ranges::any_of(values, [&](const std::string& v) { return event.Actor.ID == v; }))
                {
                    return false;
                }
            }
            else if (key == "image")
            {
                if (event.Type != "image" || !std::ranges::any_of(values, [&](const std::string& v) { return event.Actor.ID == v; }))
                {
                    return false;
                }
            }
        }
        return true;
    }

} // namespace

Microsoft::WRL::ComPtr<IWSLCEventStream> EventStore::CreateStream(
    Microsoft::WRL::ComPtr<WSLCSession> Session, uint64_t SinceTime, uint64_t UntilTime, std::map<std::string, std::vector<std::string>> Filters)
{
    // A non-zero until earlier than since describes a backwards, empty window.
    THROW_HR_WITH_USER_ERROR_IF(
        E_INVALIDARG, Localization::MessageWslcEventsInvalidTimeWindow(SinceTime, UntilTime), UntilTime != 0 && SinceTime > UntilTime);

    Microsoft::WRL::ComPtr<EventStream> stream;
    THROW_IF_FAILED(Microsoft::WRL::MakeAndInitialize<EventStream>(&stream, std::move(Session), this, SinceTime, UntilTime, std::move(Filters)));

    return stream;
}

std::optional<wsl::windows::common::wslc_schema::Event> EventStore::GetLockHeld(uint64_t SequenceNumber)
{
    // Callers resync a lagging reader before reaching here, so the requested event is never evicted.
    WI_ASSERT(SequenceNumber >= m_firstSequenceNumber);

    const uint64_t index = SequenceNumber - m_firstSequenceNumber;
    if (index >= m_events.size())
    {
        return std::nullopt;
    }

    return m_events[index];
}

bool EventStore::WaitForEvent(std::unique_lock<std::mutex>& Lock, uint64_t SequenceNumber, std::optional<uint64_t> Until)
{
    // Ready once the reader's event is buffered, its slot is evicted, or the session terminates.
    // Eviction while parked wakes us too, so the caller reports the gap on its next pass.
    const auto ready = [&] { return m_terminating || SequenceNumber < m_firstSequenceNumber + m_events.size(); };

    if (Until.has_value())
    {
        // Until is Unix seconds and system_clock shares that epoch, so it doubles as the wait deadline.
        const std::chrono::sys_seconds deadline{std::chrono::seconds{static_cast<int64_t>(Until.value())}};
        if (!m_updated.wait_until(Lock, deadline, ready))
        {
            return false;
        }
    }
    else
    {
        m_updated.wait(Lock, ready);
    }

    THROW_HR_IF(E_ABORT, m_terminating);
    return true;
}

std::optional<wsl::windows::common::wslc_schema::Event> EventStore::Get(
    std::optional<uint64_t>& SequenceNumber,
    std::optional<uint64_t> Since,
    std::optional<uint64_t> Until,
    const std::map<std::string, std::vector<std::string>>& Filters)
{
    std::unique_lock lock(m_lock);

    // Position the reader. A first read (no sequence number yet) starts at the oldest buffered
    // event
    SequenceNumber = SequenceNumber.value_or(m_firstSequenceNumber);

    while (true)
    {
        // A reader that has fallen behind the ring missed events to eviction: reset it so the
        // next call starts fresh at the oldest buffered event, and report the gap.
        if (SequenceNumber.value() < m_firstSequenceNumber)
        {
            SequenceNumber = std::nullopt;
            THROW_HR(WSLC_E_EVENTS_LOST);
        }

        if (!WaitForEvent(lock, SequenceNumber.value(), Until))
        {
            // The until window elapsed with no further event: the stream is finished.
            return std::nullopt;
        }

        // Evicted while parked: loop back to reset and report the gap.
        // TODO: A burst of more than c_eventRingCapacity events between the wake and reacquiring the
        // lock can evict this reader's event before it is read, forcing a WSLC_E_EVENTS_LOST. Redesign
        // so that every parked reader is guaranteed to observe an event before the next write can evict
        // it.
        if (SequenceNumber.value() < m_firstSequenceNumber)
        {
            continue;
        }

        const auto event = GetLockHeld(SequenceNumber.value()).value();

        // An event past the until-bound closes the window: the stream is finished.
        if (Until.has_value() && event.time > Until.value())
        {
            return std::nullopt;
        }

        // Advance past this event so the next read resumes at the following one.
        SequenceNumber.value()++;

        // Return the event if it falls within the since-bound and matches the caller's filters;
        // otherwise loop to skip it.
        if ((!Since.has_value() || event.time >= Since.value()) && EventMatchesFilters(event, Filters))
        {
            return event;
        }
    }
}

void EventStore::OnSessionTerminating()
{
    {
        std::lock_guard lock(m_lock);
        m_terminating = true;
    }

    m_updated.notify_all();
}

HRESULT EventStream::RuntimeClassInitialize(
    Microsoft::WRL::ComPtr<WSLCSession> Session,
    EventStore* Store,
    uint64_t SinceTime,
    uint64_t UntilTime,
    std::map<std::string, std::vector<std::string>> Filters)
{
    m_session = std::move(Session);
    m_store = Store;
    m_since = ToTimeBound(SinceTime);
    m_until = ToTimeBound(UntilTime);
    m_filters = std::move(Filters);
    return S_OK;
}

HRESULT EventStream::GetNext(LPSTR* EventJson)
try
{
    RETURN_HR_IF_NULL(E_POINTER, EventJson);
    *EventJson = nullptr;

    const auto event = m_store->Get(m_nextSequenceNumber, m_since, m_until, m_filters);
    if (!event.has_value())
    {
        return WSLC_E_EVENT_STREAM_FINISHED;
    }

    *EventJson = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(wsl::shared::ToJson(event.value()).c_str()).release();
    return S_OK;
}
CATCH_RETURN();

} // namespace wsl::windows::service::wslc
