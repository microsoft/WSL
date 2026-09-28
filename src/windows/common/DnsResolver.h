// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "DnsTunnelingChannel.h"
#include "WslCoreMessageQueue.h"
#include "WslCoreNetworkingSupport.h"

namespace wsl::core::networking {

enum class DnsResolverFlags
{
    None = 0x0,
    BestEffortDnsParsing = 0x1
};
DEFINE_ENUM_FLAG_OPERATORS(DnsResolverFlags);

class DnsResolver
{
public:
    DnsResolver(wil::unique_socket&& dnsHvsocket, DnsResolverFlags flags);
    ~DnsResolver() noexcept;

    DnsResolver(const DnsResolver&) = delete;
    DnsResolver& operator=(const DnsResolver&) = delete;

    DnsResolver(DnsResolver&&) = delete;
    DnsResolver& operator=(DnsResolver&&) = delete;

    void Stop() noexcept;

    static HRESULT LoadDnsResolverMethods() noexcept;

private:
    struct DnsQueryContext
    {
        // Struct containing protocol (TCP/UDP) and unique id of the Linux DNS client making the request.
        LX_GNS_DNS_CLIENT_IDENTIFIER m_dnsClientIdentifier{};

        // Handle used to cancel the request.
        DNS_QUERY_RAW_CANCEL m_cancelHandle{};

        // Unique query id.
        uint32_t m_id{};

        // Callback to the parent object to notify about the DNS query completion.
        std::function<void(DnsQueryContext*, DNS_QUERY_RAW_RESULT*)> m_handleQueryCompletion;

        DnsQueryContext(
            uint32_t id,
            const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier,
            std::function<void(DnsQueryContext*, DNS_QUERY_RAW_RESULT*)>&& handleQueryCompletion) :
            m_dnsClientIdentifier(dnsClientIdentifier), m_id(id), m_handleQueryCompletion(std::move(handleQueryCompletion))
        {
        }

        ~DnsQueryContext() noexcept = default;

        DnsQueryContext(const DnsQueryContext&) = delete;
        DnsQueryContext& operator=(const DnsQueryContext&) = delete;
        DnsQueryContext(DnsQueryContext&&) = delete;
        DnsQueryContext& operator=(DnsQueryContext&&) = delete;
    };

    void GenerateTelemetry() noexcept;

    // Process DNS request received from Linux.
    //
    // Arguments:
    // dnsBuffer - buffer containing DNS request.
    // dnsClientIdentifier - struct containing protocol (TCP/UDP) and unique id of the Linux DNS client making the request.
    void ProcessDnsRequest(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept;

    // Handle completion of DNS query.
    //
    // Arguments:
    // dnsQueryContext - context structure for the DNS request.
    // queryResults - structure containing result of the DNS request.
    void HandleDnsQueryCompletion(_Inout_ DnsQueryContext* dnsQueryContext, _Inout_opt_ DNS_QUERY_RAW_RESULT* queryResults) noexcept;

    void ResolveExternalInterfaceConstraintIndex() noexcept;

    // Callback that will be invoked by the DNS API whenever a request finishes. The callback is invoked on success, error or when request is cancelled.
    //
    // Arguments:
    // queryContext - pointer to context structure, will be a structure of type DnsQueryContext.
    // queryResults - pointer to structure containing the result of the DNS request.
    static VOID CALLBACK DnsQueryRawCallback(_In_ VOID* queryContext, _Inout_opt_ DNS_QUERY_RAW_RESULT* queryResults) noexcept;

    static VOID CALLBACK InterfaceChangeCallback(_In_ PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) noexcept;

    std::recursive_mutex m_dnsLock;

    // Flag used when shutting down the object.
    _Guarded_by_(m_dnsLock) bool m_stopped = false;

    // Hvsocket channel used to exchange DNS messages with Linux.
    DnsTunnelingChannel m_dnsChannel;

    // Queue used to send DNS responses to Linux.
    WslCoreMessageQueue m_dnsResponseQueue;

    // Unique id that is incremented for each request. In case the value reaches MAX_UINT and is reset to 0,
    // it's assumed previous requests with id's 0, 1, ... finished in the meantime and the id can be reused.
    _Guarded_by_(m_dnsLock) uint32_t m_currentRequestId = 0;

    // Mapping request id to the request context structure.
    _Guarded_by_(m_dnsLock) std::unordered_map<uint32_t, std::unique_ptr<DnsQueryContext>> m_dnsRequests {};

    // Event that is set when all tracked DNS requests have completed.
    wil::unique_event m_allRequestsFinished{wil::EventOptions::ManualReset};

    // Used for handling of external interface constraint setting.
    unique_notify_handle m_interfaceNotificationHandle{};

    std::wstring m_externalInterfaceConstraintName;
    _Guarded_by_(m_dnsLock) ULONG m_externalInterfaceConstraintIndex = 0;

    const DnsResolverFlags m_flags{};

    // Statistics used for telemetry.
    std::atomic<uint32_t> m_totalUdpQueries{0};
    std::atomic<uint32_t> m_successfulUdpQueries{0};
    std::atomic<uint32_t> m_totalTcpQueries{0};
    std::atomic<uint32_t> m_successfulTcpQueries{0};
    std::atomic<uint32_t> m_queriesWithNullResult{0};
    std::atomic<uint32_t> m_failedDnsQueryRawCalls{0};

    _Guarded_by_(m_dnsLock) std::map<uint32_t, uint32_t> m_dnsApiFailures;

    // Dynamic functions used for calling the DNS APIs.

    // Function to start a raw DNS request.
    static std::optional<LxssDynamicFunction<decltype(DnsQueryRaw)>> s_dnsQueryRaw;
    // Function to cancel a raw DNS request.
    static std::optional<LxssDynamicFunction<decltype(DnsCancelQueryRaw)>> s_dnsCancelQueryRaw;
    // Function to free the structure containing the result of a raw DNS request.
    static std::optional<LxssDynamicFunction<decltype(DnsQueryRawResultFree)>> s_dnsQueryRawResultFree;
};

} // namespace wsl::core::networking
