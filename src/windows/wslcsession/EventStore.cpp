// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "EventStore.h"
#include "WSLCSession.h"
#include "WSLCExecutionContext.h"
#include <chrono>

using wsl::shared::Localization;

namespace wsl::windows::service::wslc {

namespace {

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

    std::chrono::sys_seconds EventTime(const wsl::windows::common::wslc_schema::Event& Event)
    {
        return std::chrono::sys_seconds{std::chrono::floor<std::chrono::seconds>(std::chrono::nanoseconds{Event.timeNano})};
    }

} // namespace

void EventStore::Append(wsl::windows::common::wslc_schema::Event Event)
{
    std::lock_guard lock(m_lock);

    // Events are recorded in Docker's delivery order, which is also timestamp order to the second. Subscribers rely on
    // this: they resume from a sequence number, so an out-of-order event could never be inserted where it
    // belongs without hiding it from readers that already moved past that point.
    WI_ASSERT(m_events.empty() || EventTime(m_events.back()) <= EventTime(Event));

    m_events.push_back(std::move(Event));

    if (m_events.size() > c_eventRingCapacity)
    {
        m_events.pop_front();
        ++m_firstSequenceNumber;
    }

    m_updated.notify_all();
}

void EventStore::Record(std::string&& Type, std::string&& Action, const std::string& ActorId, std::map<std::string, std::string> ActorAttributes, std::int64_t TimeNano) noexcept
try
{
    wsl::windows::common::wslc_schema::Event event;
    event.Type = std::move(Type);
    event.Action = std::move(Action);
    event.Actor.ID = ActorId;
    event.Actor.Attributes = std::move(ActorAttributes);
    event.timeNano = TimeNano;

    Append(std::move(event));
}
CATCH_LOG()

namespace {

    // Any event matches when its actor id or name starts with a value. The type filter is what restricts the event type.
    bool MatchesIdOrName(const wsl::windows::common::wslc_schema::Event& event, const std::vector<std::string>& values)
    {
        const auto nameEntry = event.Actor.Attributes.find("name");
        const std::string_view name = nameEntry != event.Actor.Attributes.end() ? std::string_view{nameEntry->second} : std::string_view{};

        return std::ranges::any_of(
            values, [&](const std::string& value) { return event.Actor.ID.starts_with(value) || name.starts_with(value); });
    }

    // Compare the values as written against the actor id and the image name, and against the familiar form
    // of each. Image events carry their image name in "name"; other events carry it in "image".
    bool MatchesImage(const wsl::windows::common::wslc_schema::Event& event, const std::vector<std::string>& values)
    {
        const auto nameEntry = event.Actor.Attributes.find(event.Type == "image" ? "name" : "image");
        const std::string name = nameEntry != event.Actor.Attributes.end() ? nameEntry->second : std::string{};

        // A reference that doesn't parse is compared as is.
        const auto familiar = [](const std::string& image) {
            const auto reference = wsl::windows::common::wslutil::ImageReference::TryParse(image);
            return reference.has_value() ? reference->Repository.GetFamiliar() : image;
        };

        const auto matches = [&](const std::string& candidate) { return std::ranges::find(values, candidate) != values.end(); };
        return matches(event.Actor.ID) || matches(name) || matches(familiar(event.Actor.ID)) || matches(familiar(name));
    }

    // Every label filter must match. "key" requires the label to exist and "key=value" requires that exact value.
    bool MatchesLabels(const wsl::windows::common::wslc_schema::Event& event, const std::vector<std::string>& values)
    {
        return std::ranges::all_of(values, [&](const std::string& value) {
            const auto separator = value.find('=');
            const auto label = event.Actor.Attributes.find(value.substr(0, separator));

            return label != event.Actor.Attributes.end() &&
                   (separator == std::string::npos || label->second == value.substr(separator + 1));
        });
    }

    // Values sharing a key are OR'd, except label values, which must all match. Distinct keys are AND'd.
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
            else if (key == "container" || key == "network" || key == "volume")
            {
                if (!MatchesIdOrName(event, values))
                {
                    return false;
                }
            }
            else if (key == "image")
            {
                if (!MatchesImage(event, values))
                {
                    return false;
                }
            }
            else if (key == "label")
            {
                if (!MatchesLabels(event, values))
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

    static constexpr std::array c_supportedFilters{"type", "event", "container", "image", "network", "volume", "label"};
    for (const auto& [key, values] : Filters)
    {
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG,
            Localization::MessageWslcInvalidFilter(wsl::shared::string::MultiByteToWide(key)),
            std::ranges::find(c_supportedFilters, key) == c_supportedFilters.end());
    }

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
    std::unique_lock<std::mutex>& Lock, uint64_t SequenceNumber, std::optional<std::chrono::sys_seconds> Until, gsl::span<const HANDLE> WaitHandles)
{
    // Eviction also makes this true, so the caller can report the gap after waking.
    const auto eventAvailable = [&] { return SequenceNumber < m_firstSequenceNumber + m_events.size(); };
    const auto aborted = [&] {
        if (m_terminating)
        {
            return true;
        }

        if (WaitHandles.empty())
        {
            return false;
        }

        const auto result = WaitForMultipleObjects(gsl::narrow_cast<DWORD>(WaitHandles.size()), WaitHandles.data(), FALSE, 0);
        THROW_LAST_ERROR_IF(result == WAIT_FAILED);
        return result < WAIT_OBJECT_0 + WaitHandles.size();
    };
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
    std::array<HANDLE, 2> handles{};
    size_t handleCount = 0;

    // Destroy the waits after releasing m_lock and before closing the process handle. Taking
    // m_lock in the callback prevents a notification being lost between the predicate and wait.
    std::array<wil::unique_threadpool_wait, 2> waits;
    for (const auto handle : {CancelEvent, callerProcess.get()})
    {
        if (handle != nullptr)
        {
            handles[handleCount] = handle;
            waits[handleCount].reset(CreateThreadpoolWait(
                [](PTP_CALLBACK_INSTANCE, PVOID context, PTP_WAIT, TP_WAIT_RESULT) {
                    auto* store = static_cast<EventStore*>(context);
                    std::lock_guard lock(store->m_lock);
                    store->m_updated.notify_all();
                },
                this,
                nullptr));
            THROW_LAST_ERROR_IF(!waits[handleCount]);
            SetThreadpoolWait(waits[handleCount].get(), handle, nullptr);
            ++handleCount;
        }
    }

    const gsl::span<const HANDLE> waitHandles{handles.data(), handleCount};
    std::unique_lock lock(m_lock);

    // Position the reader. A first read (no sequence number yet) starts at the oldest buffered
    // event
    SequenceNumber = SequenceNumber.value_or(m_firstSequenceNumber);

    while (WaitForEvent(lock, SequenceNumber.value(), Until, waitHandles))
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

        // Compared in seconds, since converting a far-future Since or Until bound to nanoseconds would overflow.
        const auto eventTime = EventTime(event);

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
