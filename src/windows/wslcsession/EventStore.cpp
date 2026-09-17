// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "EventStore.h"
#include "WSLCSession.h"
#include "WSLCExecutionContext.h"
#include <chrono>

using wsl::shared::Localization;

namespace wsl::windows::service::wslc {

namespace {

    bool IsSignaled(HANDLE Handle)
    {
        if (Handle == nullptr)
        {
            return false;
        }

        const auto result = WaitForSingleObject(Handle, 0);
        THROW_LAST_ERROR_IF(result == WAIT_FAILED);
        return result == WAIT_OBJECT_0;
    }

    std::optional<std::chrono::sys_seconds> ToTimeBound(int64_t TimeSeconds)
    {
        if (TimeSeconds == 0)
        {
            return std::nullopt;
        }

        // Waiting on a bound converts it to the system clock's 100ns ticks, which a far-future second would overflow.
        constexpr auto c_maxBound = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::time_point::max());
        return std::min(std::chrono::sys_seconds{std::chrono::seconds{TimeSeconds}}, c_maxBound);
    }

} // namespace

void EventStore::Append(wsl::windows::common::wslc_schema::Event Event)
{
    std::lock_guard lock(m_lock);

    // Events are recorded in Docker's delivery order, which is also timestamp order. Subscribers rely on
    // this: they resume from a sequence number, so an out-of-order event could never be inserted where it
    // belongs without hiding it from readers that already moved past that point.
    WI_ASSERT(m_events.empty() || m_events.back().time <= Event.time);

    m_events.push_back(std::move(Event));

    if (m_events.size() > c_eventRingCapacity)
    {
        m_events.pop_front();
        ++m_firstSequenceNumber;
    }

    m_updated.notify_all();
}

void EventStore::Record(std::string&& Type, std::string&& Action, const std::string& ActorId, std::map<std::string, std::string> ActorAttributes, std::int64_t Time) noexcept
try
{
    wsl::windows::common::wslc_schema::Event event;
    event.Type = std::move(Type);
    event.Action = std::move(Action);
    event.Actor.ID = ActorId;
    event.Actor.Attributes = std::move(ActorAttributes);
    event.time = Time;

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
                const auto image = event.Actor.Attributes.find("image");
                const bool matches = std::ranges::any_of(values, [&](const std::string& value) {
                    return (event.Type == "image" && event.Actor.ID == value) ||
                           (event.Type == "container" && image != event.Actor.Attributes.end() && image->second == value);
                });
                if (!matches)
                {
                    return false;
                }
            }
            else if (key == "network")
            {
                const auto nameEntry = event.Actor.Attributes.find("name");
                const std::string_view name =
                    nameEntry != event.Actor.Attributes.end() ? std::string_view{nameEntry->second} : std::string_view{};

                // Docker matches a network against its id or its name, either in full or by prefix.
                const auto matchesIdOrName = [&](const std::string& value) {
                    return event.Actor.ID.starts_with(value) || name.starts_with(value);
                };

                if (event.Type != "network" || !std::ranges::any_of(values, matchesIdOrName))
                {
                    return false;
                }
            }
        }
        return true;
    }

} // namespace

Microsoft::WRL::ComPtr<IWSLCEventStream> EventStore::CreateStream(
    Microsoft::WRL::ComPtr<WSLCSession> Session, int64_t SinceTime, int64_t UntilTime, std::map<std::string, std::vector<std::string>> Filters)
{
    // Zero means unbounded on that end, so it never makes the window run backwards.
    THROW_HR_WITH_USER_ERROR_IF(
        E_INVALIDARG,
        Localization::MessageWslcEventsInvalidTimeWindow(SinceTime, UntilTime),
        SinceTime < 0 || UntilTime < 0 || (SinceTime != 0 && UntilTime != 0 && SinceTime > UntilTime));

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

bool EventStore::WaitForEvent(
    std::unique_lock<std::mutex>& Lock, uint64_t SequenceNumber, std::optional<std::chrono::sys_seconds> Until, HANDLE CancelEvent, HANDLE CallerProcess)
{
    // Eviction also makes this true, so the caller can report the gap after waking.
    const auto eventAvailable = [&] { return SequenceNumber < m_firstSequenceNumber + m_events.size(); };
    const auto aborted = [&] { return m_terminating || IsSignaled(CancelEvent) || IsSignaled(CallerProcess); };
    const auto ready = [&] { return aborted() || eventAvailable(); };

    if (Until.has_value())
    {
        if (!m_updated.wait_until(Lock, Until.value(), ready))
        {
            return false;
        }
    }
    else
    {
        m_updated.wait(Lock, ready);
    }

    THROW_HR_IF(E_ABORT, aborted());
    return true;
}

std::optional<wsl::windows::common::wslc_schema::Event> EventStore::Get(
    std::optional<uint64_t>& SequenceNumber,
    std::optional<std::chrono::sys_seconds> Since,
    std::optional<std::chrono::sys_seconds> Until,
    const std::map<std::string, std::vector<std::string>>& Filters,
    HANDLE CancelEvent)
{
    const auto callerProcess = wsl::windows::common::wslutil::OpenCallingProcess(SYNCHRONIZE);
    const std::array handles{CancelEvent, callerProcess.get()};

    // Destroy the waits after releasing m_lock and before closing the process handle. Taking
    // m_lock in the callback prevents a notification being lost between the predicate and wait.
    std::array<wil::unique_threadpool_wait, 2> waits;
    for (size_t i = 0; i < handles.size(); ++i)
    {
        if (handles[i] != nullptr)
        {
            waits[i].reset(CreateThreadpoolWait(
                [](PTP_CALLBACK_INSTANCE, PVOID context, PTP_WAIT, TP_WAIT_RESULT) {
                    auto* store = static_cast<EventStore*>(context);
                    std::lock_guard lock(store->m_lock);
                    store->m_updated.notify_all();
                },
                this,
                nullptr));
            THROW_LAST_ERROR_IF(!waits[i]);
            SetThreadpoolWait(waits[i].get(), handles[i], nullptr);
        }
    }

    std::unique_lock lock(m_lock);

    // Position the reader. A first read (no sequence number yet) starts at the oldest buffered
    // event
    SequenceNumber = SequenceNumber.value_or(m_firstSequenceNumber);

    while (WaitForEvent(lock, SequenceNumber.value(), Until, CancelEvent, callerProcess.get()))
    {
        // A reader that has fallen behind the ring missed events to eviction: reset it so the
        // next call starts fresh at the oldest buffered event, and report the gap.
        if (SequenceNumber.value() < m_firstSequenceNumber)
        {
            SequenceNumber = std::nullopt;
            THROW_HR(WSLC_E_EVENTS_LOST);
        }

        // TODO: A burst of more than c_eventRingCapacity events between the wake and reacquiring the
        // lock can evict this reader's event before it is read, forcing a WSLC_E_EVENTS_LOST. Redesign
        // so that every parked reader is guaranteed to observe an event before the next write can evict
        // it.
        const auto event = GetLockHeld(SequenceNumber.value()).value();
        const std::chrono::sys_seconds eventTime{std::chrono::seconds{event.time}};

        // Advance in delivery order before applying the time window.
        SequenceNumber.value()++;

        // Events are appended in non-decreasing timestamp order (see Append()), so once we reach the
        // exclusive Until bound, the stream is finished.
        if (Until.has_value() && eventTime >= Until.value())
        {
            return std::nullopt;
        }

        // Return the event if it falls within the since-bound and matches the caller's filters;
        // otherwise loop to skip it.
        if ((!Since.has_value() || eventTime >= Since.value()) && EventMatchesFilters(event, Filters))
        {
            return event;
        }
    }

    return std::nullopt;
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
    int64_t SinceTime,
    int64_t UntilTime,
    std::map<std::string, std::vector<std::string>> Filters)
{
    m_session = std::move(Session);
    m_store = Store;
    m_since = ToTimeBound(SinceTime);
    m_until = ToTimeBound(UntilTime);
    m_filters = std::move(Filters);
    return S_OK;
}

HRESULT EventStream::GetNext(HANDLE CancelEvent, LPSTR* EventJson)
try
{
    RETURN_HR_IF_NULL(E_POINTER, EventJson);
    *EventJson = nullptr;

    std::lock_guard lock(m_lock);
    const auto event = m_store->Get(m_nextSequenceNumber, m_since, m_until, m_filters, CancelEvent);
    if (!event.has_value())
    {
        return WSLC_E_EVENT_STREAM_FINISHED;
    }

    *EventJson = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(wsl::shared::ToJson(event.value()).c_str()).release();
    return S_OK;
}
CATCH_RETURN();

} // namespace wsl::windows::service::wslc
