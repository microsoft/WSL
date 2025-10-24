// Copyright (C) Microsoft Corporation. All rights reserved.

#include <LxssDynamicFunction.h>
#include "precomp.h"
#include "DnsResolver.h"

using wsl::core::networking::DnsResolver;

static constexpr auto c_dnsModuleName = L"dnsapi.dll";

std::optional<LxssDynamicFunction<decltype(DnsQueryRaw)>> DnsResolver::s_dnsQueryRaw;
std::optional<LxssDynamicFunction<decltype(DnsCancelQueryRaw)>> DnsResolver::s_dnsCancelQueryRaw;
std::optional<LxssDynamicFunction<decltype(DnsQueryRawResultFree)>> DnsResolver::s_dnsQueryRawResultFree;

HRESULT DnsResolver::LoadDnsResolverMethods() noexcept
{
    static wil::shared_hmodule dnsModule;
    static DWORD loadError = ERROR_SUCCESS;
    static std::once_flag dnsLoadFlag;

    // Load DNS dll only once
    std::call_once(dnsLoadFlag, [&]() {
        dnsModule.reset(LoadLibraryEx(c_dnsModuleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
        if (!dnsModule)
        {
            loadError = GetLastError();
        }
    });

    RETURN_IF_WIN32_ERROR_MSG(loadError, "LoadLibraryEx %ls", c_dnsModuleName);

    // Initialize dynamic functions for the DNS tunneling Windows APIs.
    // using the non-throwing instance of LxssDynamicFunction as to not end up in the Error telemetry
    LxssDynamicFunction<decltype(DnsQueryRaw)> local_dnsQueryRaw{DynamicFunctionErrorLogs::None};
    RETURN_IF_FAILED_EXPECTED(local_dnsQueryRaw.load(dnsModule, "DnsQueryRaw"));
    LxssDynamicFunction<decltype(DnsCancelQueryRaw)> local_dnsCancelQueryRaw{DynamicFunctionErrorLogs::None};
    RETURN_IF_FAILED_EXPECTED(local_dnsCancelQueryRaw.load(dnsModule, "DnsCancelQueryRaw"));
    LxssDynamicFunction<decltype(DnsQueryRawResultFree)> local_dnsQueryRawResultFree{DynamicFunctionErrorLogs::None};
    RETURN_IF_FAILED_EXPECTED(local_dnsQueryRawResultFree.load(dnsModule, "DnsQueryRawResultFree"));

    // Make a dummy call to the DNS APIs to verify if they are working. The APIs are going to be present
    // on older Windows versions, where they can be turned on/off. If turned off, the APIs
    // will be unusable and will return ERROR_CALL_NOT_IMPLEMENTED.
    if (local_dnsQueryRaw(nullptr, nullptr) == ERROR_CALL_NOT_IMPLEMENTED)
    {
        RETURN_IF_WIN32_ERROR_EXPECTED(ERROR_CALL_NOT_IMPLEMENTED);
    }

    s_dnsQueryRaw.emplace(std::move(local_dnsQueryRaw));
    s_dnsCancelQueryRaw.emplace(std::move(local_dnsCancelQueryRaw));
    s_dnsQueryRawResultFree.emplace(std::move(local_dnsQueryRawResultFree));
    return S_OK;
}

DnsResolver::DnsResolver(wil::unique_socket&& dnsHvsocket, DnsResolverFlags flags) :
    m_dnsChannel(
        std::move(dnsHvsocket),
        [this](const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) {
            ProcessDnsRequest(dnsBuffer, dnsClientIdentifier);
        }),
    m_flags(flags)
{
    // Initialize as signaled, as there are no requests yet
    m_allRequestsFinished.SetEvent();

    // Read external interface constraint regkey
    const auto lxssKey = windows::common::registry::OpenLxssMachineKey(KEY_READ);
    m_externalInterfaceConstraintName =
        windows::common::registry::ReadString(lxssKey.get(), nullptr, c_interfaceConstraintKey, L"");

    if (!m_externalInterfaceConstraintName.empty())
    {
        ResolveExternalInterfaceConstraintIndex();

        WSL_LOG(
            "DnsResolver::DnsResolver",
            TraceLoggingValue(m_externalInterfaceConstraintName.c_str(), "m_externalInterfaceConstraintName"),
            TraceLoggingValue(m_externalInterfaceConstraintIndex, "m_externalInterfaceConstraintIndex"));

        // Register for interface change notifications. Notifications are used to determine if the external interface constraint setting is applicable.
        THROW_IF_WIN32_ERROR(NotifyIpInterfaceChange(AF_UNSPEC, &DnsResolver::InterfaceChangeCallback, this, FALSE, &m_interfaceNotificationHandle));
    }
}

DnsResolver::~DnsResolver() noexcept
{
    Stop();
}

void DnsResolver::GenerateTelemetry() noexcept
try
{
    // Find the 3 most common DNS API failures
    uint32_t mostCommonDnsStatusError = 0;
    uint32_t mostCommonDnsStatusErrorCount = 0;
    uint32_t secondCommonDnsStatusError = 0;
    uint32_t secondCommonDnsStatusErrorCount = 0;
    uint32_t thirdCommonDnsStatusError = 0;
    uint32_t thirdCommonDnsStatusErrorCount = 0;

    std::vector<std::pair<uint32_t, uint32_t>> failures(m_dnsApiFailures.size());
    std::copy(m_dnsApiFailures.begin(), m_dnsApiFailures.end(), failures.begin());

    // Sort in descending order based on failure count
    std::sort(failures.begin(), failures.end(), [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

    if (failures.size() >= 1)
    {
        mostCommonDnsStatusError = failures[0].first;
        mostCommonDnsStatusErrorCount = failures[0].second;
    }
    if (failures.size() >= 2)
    {
        secondCommonDnsStatusError = failures[1].first;
        secondCommonDnsStatusErrorCount = failures[1].second;
    }
    if (failures.size() >= 3)
    {
        thirdCommonDnsStatusError = failures[2].first;
        thirdCommonDnsStatusErrorCount = failures[2].second;
    }

    // Add telemetry with DNS tunneling statistics, before shutting down
    WSL_LOG(
        "DnsTunnelingStatistics",
        TraceLoggingValue(m_totalUdpQueries.load(), "totalUdpQueries"),
        TraceLoggingValue(m_successfulUdpQueries.load(), "successfulUdpQueries"),
        TraceLoggingValue(m_totalTcpQueries.load(), "totalTcpQueries"),
        TraceLoggingValue(m_successfulTcpQueries.load(), "successfulTcpQueries"),
        TraceLoggingValue(m_queriesWithNullResult.load(), "queriesWithNullResult"),
        TraceLoggingValue(m_failedDnsQueryRawCalls.load(), "FailedDnsQueryRawCalls"),
        TraceLoggingValue(m_dnsApiFailures.size(), "totalDnsStatusErrorInstances"),
        TraceLoggingValue(mostCommonDnsStatusError, "mostCommonDnsStatusError"),
        TraceLoggingValue(mostCommonDnsStatusErrorCount, "mostCommonDnsStatusErrorCount"),
        TraceLoggingValue(secondCommonDnsStatusError, "secondCommonDnsStatusError"),
        TraceLoggingValue(secondCommonDnsStatusErrorCount, "secondCommonDnsStatusErrorCount"),
        TraceLoggingValue(thirdCommonDnsStatusError, "thirdCommonDnsStatusError"),
        TraceLoggingValue(thirdCommonDnsStatusErrorCount, "thirdCommonDnsStatusErrorCount"));
}
CATCH_LOG()

void DnsResolver::Stop() noexcept
try
{
    WSL_LOG("DnsResolver::Stop");

    // Scoped m_dnsLock
    {
        const std::lock_guard lock(m_dnsLock);

        m_stopped = true;

        // Cancel existing requests. Cancel is complete when DnsQueryRawCallback is
        // invoked with status == ERROR_CANCELLED
        // N.B. Cancelling can end up calling the DnsQueryRawCallback directly on this same thread. i.e., while this
        // lock is held. Which is fine because m_dnsLock is a recursive mutex.
        // N.B. Cancelling a query will synchronously remove the query from m_dnsRequests, which invalidates iterators.

        std::vector<DNS_QUERY_RAW_CANCEL*> cancelHandles;
        cancelHandles.reserve(m_dnsRequests.size());

        for (auto& [_, context] : m_dnsRequests)
        {
            cancelHandles.emplace_back(&context->m_cancelHandle);
        }

        for (const auto e : cancelHandles)
        {
            LOG_IF_WIN32_ERROR(s_dnsCancelQueryRaw.value()(e));
        }
    }

    // Wait for all requests to complete. At this point no new requests can be started since the object is stopped.
    // We are only waiting for existing requests to finish.
    m_allRequestsFinished.wait();

    // Stop the response queue first as it can make calls in m_dnsChannel
    m_dnsResponseQueue.cancel();

    m_dnsChannel.Stop();

    // Stop interface change notifications
    m_interfaceNotificationHandle.reset();

    GenerateTelemetry();
}
CATCH_LOG()

void DnsResolver::ProcessDnsRequest(const gsl::span<gsl::byte> dnsBuffer, const LX_GNS_DNS_CLIENT_IDENTIFIER& dnsClientIdentifier) noexcept
try
{
    const std::lock_guard lock(m_dnsLock);
    if (m_stopped)
    {
        return;
    }

    WSL_LOG_DEBUG(
        "DnsResolver::ProcessDnsRequest - received new DNS request",
        TraceLoggingValue(dnsBuffer.size(), "DNS buffer size"),
        TraceLoggingValue(dnsClientIdentifier.Protocol == IPPROTO_UDP ? "UDP" : "TCP", "Protocol"),
        TraceLoggingValue(dnsClientIdentifier.DnsClientId, "DNS client id"),
        TraceLoggingValue(!m_externalInterfaceConstraintName.empty(), "Is ExternalInterfaceConstraint configured"),
        TraceLoggingValue(m_externalInterfaceConstraintIndex, "m_externalInterfaceConstraintIndex"));

    // If the external interface constraint is configured but it is *not* present/up, WSL should be net-blind, so we avoid making DNS requests.
    if (!m_externalInterfaceConstraintName.empty() && m_externalInterfaceConstraintIndex == 0)
    {
        return;
    }

    dnsClientIdentifier.Protocol == IPPROTO_UDP ? m_totalUdpQueries++ : m_totalTcpQueries++;

    // Get next request id. If value reaches UINT_MAX + 1 it will be automatically reset to 0
    const auto requestId = m_currentRequestId++;

    // Create the DNS request context
    auto context = std::make_unique<DnsResolver::DnsQueryContext>(
        requestId, dnsClientIdentifier, [this](_Inout_ DnsResolver::DnsQueryContext* context, _Inout_opt_ DNS_QUERY_RAW_RESULT* queryResults) {
            HandleDnsQueryCompletion(context, queryResults);
        });

    auto [it, _] = m_dnsRequests.emplace(requestId, std::move(context));
    const auto localContext = it->second.get();

    auto removeContextOnError = wil::scope_exit([&] { WI_VERIFY(m_dnsRequests.erase(requestId) == 1); });

    // Fill DNS request structure
    DNS_QUERY_RAW_REQUEST request{};

    request.version = DNS_QUERY_RAW_REQUEST_VERSION1;
    request.resultsVersion = DNS_QUERY_RAW_RESULTS_VERSION1;
    request.dnsQueryRawSize = static_cast<ULONG>(dnsBuffer.size());
    request.dnsQueryRaw = (PBYTE)dnsBuffer.data();
    request.protocol = (dnsClientIdentifier.Protocol == IPPROTO_TCP) ? DNS_PROTOCOL_TCP : DNS_PROTOCOL_UDP;
    request.queryCompletionCallback = DnsResolver::DnsQueryRawCallback;
    request.queryContext = localContext;
    // Only unicast UDP & TCP queries are tunneled. Pass this flag to tell Windows DNS client to *not* resolve using multicast.
    request.queryOptions |= DNS_QUERY_NO_MULTICAST;

    // In a DNS request from Linux there might be DNS records that Windows DNS client does not know how to parse.
    // By default in this case Windows will fail the request. When the flag is enabled, Windows will extract the
    // question from the DNS request and attempt to resolve it, ignoring the unknown records.
    if (WI_IsFlagSet(m_flags, DnsResolverFlags::BestEffortDnsParsing))
    {
        request.queryRawOptions |= DNS_QUERY_RAW_OPTION_BEST_EFFORT_PARSE;
    }

    // If the external interface constraint is configured and present on the host, only send DNS requests on that interface.
    if (m_externalInterfaceConstraintIndex != 0)
    {
        request.interfaceIndex = m_externalInterfaceConstraintIndex;
    }

    // Start the DNS request
    // N.B. All DNS requests will bypass the Windows DNS cache
    const auto result = s_dnsQueryRaw.value()(&request, &localContext->m_cancelHandle);
    if (result != DNS_REQUEST_PENDING)
    {
        m_failedDnsQueryRawCalls++;

        WSL_LOG(
            "ProcessDnsRequestFailed",
            TraceLoggingValue(requestId, "requestId"),
            TraceLoggingValue(result, "result"),
            TraceLoggingValue("DnsQueryRaw", "executionStep"));
        return;
    }

    removeContextOnError.release();

    m_allRequestsFinished.ResetEvent();
}
CATCH_LOG()

void DnsResolver::HandleDnsQueryCompletion(_Inout_ DnsResolver::DnsQueryContext* queryContext, _Inout_opt_ DNS_QUERY_RAW_RESULT* queryResults) noexcept
try
{
    // Always free the query result structure
    const auto freeQueryResults = wil::scope_exit([&] {
        if (queryResults != nullptr)
        {
            s_dnsQueryRawResultFree.value()(queryResults);
        }
    });

    const std::lock_guard lock(m_dnsLock);

    if (queryResults != nullptr)
    {
        WSL_LOG(
            "DnsResolver::HandleDnsQueryCompletion",
            TraceLoggingValue(queryContext->m_id, "queryContext->m_id"),
            TraceLoggingValue(queryResults->queryStatus, "queryResults->queryStatus"),
            TraceLoggingValue(queryResults->queryRawResponse != nullptr, "validResponse"));

        // Note: The response may be valid even if queryResults->queryStatus is not 0, for example when the DNS server returns a negative response.
        if (queryResults->queryRawResponse != nullptr)
        {
            queryContext->m_dnsClientIdentifier.Protocol == IPPROTO_UDP ? m_successfulUdpQueries++ : m_successfulTcpQueries++;
        }
        // the Windows DNS API returned failure
        else
        {
            if (m_dnsApiFailures.find(queryResults->queryStatus) == m_dnsApiFailures.end())
            {
                m_dnsApiFailures[queryResults->queryStatus] = 1;
            }
            else
            {
                m_dnsApiFailures[queryResults->queryStatus]++;
            }
        }
    }
    else
    {
        WSL_LOG(
            "DnsResolver::HandleDnsQueryCompletion - received a NULL queryResults",
            TraceLoggingValue(queryContext->m_id, "queryContext->m_id"));
        m_queriesWithNullResult++;
    }

    if (!m_stopped && queryResults != nullptr && queryResults->queryRawResponse != nullptr)
    {
        // Copy DNS response buffer
        std::vector<gsl::byte> dnsResponse(queryResults->queryRawResponseSize);
        CopyMemory(dnsResponse.data(), queryResults->queryRawResponse, queryResults->queryRawResponseSize);

        WSL_LOG_DEBUG(
            "DnsResolver::HandleDnsQueryCompletion - received new DNS response",
            TraceLoggingValue(dnsResponse.size(), "DNS buffer size"),
            TraceLoggingValue(queryContext->m_dnsClientIdentifier.Protocol == IPPROTO_UDP ? "UDP" : "TCP", "Protocol"),
            TraceLoggingValue(queryContext->m_dnsClientIdentifier.DnsClientId, "DNS client id"));

        // Schedule the DNS response to be sent to Linux
        m_dnsResponseQueue.submit([this, dnsResponse = std::move(dnsResponse), dnsClientIdentifier = queryContext->m_dnsClientIdentifier]() mutable {
            m_dnsChannel.SendDnsMessage(gsl::make_span(dnsResponse), dnsClientIdentifier);
        });
    }

    // Stop tracking this DNS request and delete the request context
    WI_VERIFY(m_dnsRequests.erase(queryContext->m_id) == 1);

    // Set event if all tracked requests have finished
    if (m_dnsRequests.empty())
    {
        m_allRequestsFinished.SetEvent();
    }
}
CATCH_LOG()

void DnsResolver::ResolveExternalInterfaceConstraintIndex() noexcept
try
{
    const std::lock_guard lock(m_dnsLock);
    if (m_stopped)
    {
        return;
    }

    if (m_externalInterfaceConstraintName.empty())
    {
        return;
    }

    NET_LUID interfaceLuid{};
    ULONG interfaceIndex = 0;

    // Update the interface index on every exit path.
    // The calls below to convert interface name to index will fail if the interface does not exist anymore,
    // in which case we still need to reset the interface index to its default value of 0.
    const auto setInterfaceIndex = wil::scope_exit([&] {
        if (interfaceIndex != m_externalInterfaceConstraintIndex)
        {
            WSL_LOG(
                "DnsResolver::ResolveExternalInterfaceConstraintIndex - setting m_externalInterfaceConstraintIndex to new value",
                TraceLoggingValue(m_externalInterfaceConstraintIndex, "old interface index"),
                TraceLoggingValue(interfaceIndex, "new interface index"));

            m_externalInterfaceConstraintIndex = interfaceIndex;
        }
    });

    // If external interface constraint is configured, query to see if it's present on the host.
    auto errorCode = ConvertInterfaceAliasToLuid(m_externalInterfaceConstraintName.c_str(), &interfaceLuid);
    if (FAILED_WIN32_LOG(errorCode))
    {
        return;
    }

    errorCode = ConvertInterfaceLuidToIndex(&interfaceLuid, reinterpret_cast<PNET_IFINDEX>(&interfaceIndex));
    if (FAILED_WIN32_LOG(errorCode))
    {
        return;
    }
}
CATCH_LOG()

VOID CALLBACK DnsResolver::DnsQueryRawCallback(_In_ VOID* queryContext, _Inout_opt_ DNS_QUERY_RAW_RESULT* queryResults) noexcept
try
{
    assert(queryContext != nullptr);

    const auto context = static_cast<DnsQueryContext*>(queryContext);

    // Call into DnsResolver parent object to process the query result
    context->m_handleQueryCompletion(context, queryResults);
}
CATCH_LOG()

VOID CALLBACK DnsResolver::InterfaceChangeCallback(_In_ PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) noexcept
try
{
    const auto dnsResolver = static_cast<DnsResolver*>(context);
    dnsResolver->ResolveExternalInterfaceConstraintIndex();
}
CATCH_LOG()
