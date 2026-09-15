// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "wslc.h"
#include "wslc_schema.h"

namespace wsl::windows::service::wslc {

class WSLCSession;

class EventStore
{
public:
    static constexpr size_t c_eventRingCapacity = 256;

    void Record(std::string&& Type, std::string&& Action, const std::string& ActorId, std::map<std::string, std::string> ActorAttributes, std::int64_t Time) noexcept;

    Microsoft::WRL::ComPtr<IWSLCEventStream> CreateStream(
        Microsoft::WRL::ComPtr<WSLCSession> Session, int64_t SinceTime, int64_t UntilTime, std::map<std::string, std::vector<std::string>> Filters);

    // Returns the next event at or after SequenceNumber that falls within [Since, Until) and matches
    // Filters, advancing SequenceNumber past it. A nullopt SequenceNumber starts a fresh reader at the
    // oldest buffered event (no gap is reported). If the reader has since fallen behind the ring,
    // resyncs SequenceNumber to the oldest buffered event and throws WSLC_E_EVENTS_LOST. Returns
    // nullopt once the Until window has closed.
    std::optional<wsl::windows::common::wslc_schema::Event> Get(
        std::optional<uint64_t>& SequenceNumber,
        std::optional<std::chrono::sys_seconds> Since,
        std::optional<std::chrono::sys_seconds> Until,
        const std::map<std::string, std::vector<std::string>>& Filters);

    void OnSessionTerminating();

private:
    void Append(wsl::windows::common::wslc_schema::Event Event);

    // Blocks until the event at SequenceNumber is buffered, its slot is evicted, or the session
    // terminates. Returns false only when Until elapsed with no event ready. Throws E_ABORT if the
    // session terminated while waiting.
    bool WaitForEvent(std::unique_lock<std::mutex>& Lock, uint64_t SequenceNumber, std::optional<std::chrono::sys_seconds> Until);

    std::optional<wsl::windows::common::wslc_schema::Event> GetLockHeld(uint64_t SequenceNumber);

    std::mutex m_lock;
    std::condition_variable m_updated;

    _Guarded_by_(m_lock) std::deque<wsl::windows::common::wslc_schema::Event> m_events;
    _Guarded_by_(m_lock) uint64_t m_firstSequenceNumber = 1;

    _Guarded_by_(m_lock) bool m_terminating = false;
};

class EventStream
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCEventStream, IFastRundown>
{
public:
    HRESULT RuntimeClassInitialize(
        Microsoft::WRL::ComPtr<WSLCSession> Session,
        EventStore* Store,
        int64_t SinceTime,
        int64_t UntilTime,
        std::map<std::string, std::vector<std::string>> Filters);

    IFACEMETHOD(GetNext)(_Outptr_result_z_ LPSTR* EventJson) override;

private:
    Microsoft::WRL::ComPtr<WSLCSession> m_session;
    EventStore* m_store = nullptr;

    std::optional<std::chrono::sys_seconds> m_since;
    std::optional<std::chrono::sys_seconds> m_until;
    std::map<std::string, std::vector<std::string>> m_filters;

    std::mutex m_lock;
    _Guarded_by_(m_lock) std::optional<uint64_t> m_nextSequenceNumber;
};

} // namespace wsl::windows::service::wslc
