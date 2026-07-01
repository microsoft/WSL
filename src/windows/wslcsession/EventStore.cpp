// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "EventStore.h"
#include "WSLCSession.h"
#include <chrono>
#include "wslc_schema.h"

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
    // Start the reader at the oldest event still buffered so it sees all the buffered history.
    uint64_t startSequenceNumber;
    {
        std::lock_guard lock(m_lock);
        startSequenceNumber = m_firstSequenceNumber;
    }

    Microsoft::WRL::ComPtr<EventStream> stream;
    THROW_IF_FAILED(Microsoft::WRL::MakeAndInitialize<EventStream>(
        &stream, std::move(Session), this, startSequenceNumber, SinceTime, UntilTime, std::move(Filters)));

    return stream;
}

std::optional<wsl::windows::common::wslc_schema::Event> EventStore::GetLockHeld(uint64_t SequenceNumber)
{
    if (SequenceNumber < m_firstSequenceNumber)
    {
        THROW_HR(WSLC_E_EVENTS_LOST);
    }

    const uint64_t index = SequenceNumber - m_firstSequenceNumber;
    if (index >= m_events.size())
    {
        return std::nullopt;
    }

    return m_events[index];
}

std::optional<wsl::windows::common::wslc_schema::Event> EventStore::Get(uint64_t SequenceNumber, std::optional<uint64_t> Until)
{
    std::unique_lock lock(m_lock);

    // Checked before parking, so a termination that already latched is seen without sleeping.
    const auto ready = [&] { return m_terminating || GetLockHeld(SequenceNumber).has_value(); };

    if (Until.has_value())
    {
        // The bound is Unix seconds and system_clock shares that epoch, so it doubles as a wall-clock
        // deadline. Round it up to system_clock's finer tick so the wait never ends early.
        const std::chrono::sys_seconds untilTime{std::chrono::seconds{static_cast<int64_t>(Until.value())}};
        const auto deadline = std::chrono::ceil<std::chrono::system_clock::duration>(untilTime);
        if (!m_updated.wait_until(lock, deadline, ready))
        {
            return std::nullopt;
        }
    }
    else
    {
        m_updated.wait(lock, ready);
    }

    THROW_HR_IF(E_ABORT, m_terminating);
    return GetLockHeld(SequenceNumber);
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
    uint64_t StartSequenceNumber,
    uint64_t SinceTime,
    uint64_t UntilTime,
    std::map<std::string, std::vector<std::string>> Filters)
{
    m_session = std::move(Session);
    m_store = Store;
    m_nextSequenceNumber = StartSequenceNumber;
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

    // Read forward until an event lands inside the time window and matches the filters.
    while (true)
    {
        const auto event = m_store->Get(m_nextSequenceNumber, m_until);
        if (!event.has_value())
        {
            return WSLC_E_EVENT_STREAM_FINISHED;
        }

        ++m_nextSequenceNumber;

        const uint64_t eventTime = event.value().time;

        if (m_until.has_value() && eventTime > m_until.value())
        {
            return WSLC_E_EVENT_STREAM_FINISHED;
        }

        if (m_since.has_value() && eventTime < m_since.value())
        {
            continue;
        }

        if (!EventMatchesFilters(event.value(), m_filters))
        {
            continue;
        }

        *EventJson = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(wsl::shared::ToJson(event.value()).c_str()).release();
        return S_OK;
    }
}
CATCH_RETURN();

} // namespace wsl::windows::service::wslc
