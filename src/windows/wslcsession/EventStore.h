// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

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

    void Record(std::string Type, std::string Action, const std::string& ActorId, std::optional<uint64_t> TimeSeconds = std::nullopt) noexcept;

    Microsoft::WRL::ComPtr<IWSLCEventStream> CreateStream(
        Microsoft::WRL::ComPtr<WSLCSession> Session, uint64_t SinceTime, uint64_t UntilTime, std::map<std::string, std::vector<std::string>> Filters);

    // Returns the next event at or after SequenceNumber that falls within [Since, Until] and matches
    // Filters, advancing SequenceNumber past it. A nullopt SequenceNumber starts a fresh reader at the
    // oldest buffered event (no gap is reported). If the reader has since fallen behind the ring,
    // resyncs SequenceNumber to the oldest buffered event and throws WSLC_E_EVENTS_LOST. Returns
    // nullopt once the Until window has closed.
    std::optional<wsl::windows::common::wslc_schema::Event> Get(
        std::optional<uint64_t>& SequenceNumber,
        std::optional<uint64_t> Since,
        std::optional<uint64_t> Until,
        const std::map<std::string, std::vector<std::string>>& Filters);

    void OnSessionTerminating();

private:
    void Append(wsl::windows::common::wslc_schema::Event Event);

    // Blocks until the event at SequenceNumber is buffered, its slot is evicted, or the session
    // terminates. Returns false only when Until (Unix seconds) elapsed with no event ready. Throws
    // E_ABORT if the session terminated while waiting.
    bool WaitForEvent(std::unique_lock<std::mutex>& Lock, uint64_t SequenceNumber, std::optional<uint64_t> Until);

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
        uint64_t SinceTime,
        uint64_t UntilTime,
        std::map<std::string, std::vector<std::string>> Filters);

    IFACEMETHOD(GetNext)(_Outptr_result_z_ LPSTR* EventJson) override;

private:
    Microsoft::WRL::ComPtr<WSLCSession> m_session;
    EventStore* m_store = nullptr;

    std::optional<uint64_t> m_since;
    std::optional<uint64_t> m_until;
    std::map<std::string, std::vector<std::string>> m_filters;

    std::optional<uint64_t> m_nextSequenceNumber;
};

} // namespace wsl::windows::service::wslc
