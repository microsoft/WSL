// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <map>
#include <set>
#include <utility>
#include <optional>
#include <NetlinkChannel.h>
#include <future>
#include <functional>
#include <memory>
#include <time.h>
#include "util.h"
#include <linux/seccomp.h>
#include "waitablevalue.h"
#include "SecCompDispatcher.h"
#include "SocketChannel.h"

class GnsPortTracker
{
public:
    GnsPortTracker(std::shared_ptr<wsl::shared::SocketChannel> hvSocketChannel, NetlinkChannel&& netlinkChannel, std::shared_ptr<SecCompDispatcher> seccompDispatcher);

    GnsPortTracker(const GnsPortTracker&) = delete;
    GnsPortTracker(GnsPortTracker&&) = delete;
    GnsPortTracker& operator=(const GnsPortTracker&) = delete;
    GnsPortTracker& operator=(GnsPortTracker&&) = delete;

    void Run();

    int ProcessSecCompNotification(seccomp_notif* notification);

    struct PortAllocation
    {
        in6_addr Address = {};
        std::uint16_t Port = {};
        int Family = {};
        int Protocol = {};

        PortAllocation(PortAllocation&&) = default;
        PortAllocation(const PortAllocation&) = default;

        PortAllocation& operator=(PortAllocation&&) = default;
        PortAllocation& operator=(const PortAllocation&) = default;

        PortAllocation(std::uint16_t Port, int Family, int Protocol, in6_addr& Address) :
            Port(Port), Family(Family), Protocol(Protocol)
        {
            memcpy(this->Address.s6_addr32, Address.s6_addr32, sizeof(this->Address.s6_addr32));
        }

        bool operator<(const PortAllocation& other) const
        {
            if (Port < other.Port)
            {
                return true;
            }
            else if (Port > other.Port)
            {
                return false;
            }

            if (Family < other.Family)
            {
                return true;
            }
            else if (Family > other.Family)
            {
                return false;
            }

            if (Protocol < other.Protocol)
            {
                return true;
            }
            else if (Protocol > other.Protocol)
            {
                return false;
            }

            static_assert(sizeof(Address.s6_addr32) == 16);
            if (int res = memcmp(Address.s6_addr32, other.Address.s6_addr32, sizeof(Address.s6_addr32)); res < 0)
            {
                return true;
            }
            else if (res > 0)
            {
                return false;
            }

            return false;
        }
    };

    struct BindCall
    {
        std::optional<PortAllocation> Request;
        std::uint64_t CallId;
    };

    struct PortRefreshResult
    {
        std::set<PortAllocation> Ports;
        time_t Timestamp;
        std::function<void()> Resume;
    };

private:
    void OnRefreshAllocatedPorts(const std::set<PortAllocation>& Ports, time_t Timestamp);

    void RunPortRefresh();

    std::set<PortAllocation> ListAllocatedPorts();

    std::optional<BindCall> ReadNextRequest();

    std::optional<BindCall> GetCallInfo(uint64_t CallId, pid_t Pid, int Arch, int SysCallNumber, const gsl::span<unsigned long long>& Arguments);

    int RequestPort(const PortAllocation& Port, bool Allocate);

    int ClosePort(const PortAllocation& Port);

    int HandleRequest(const PortAllocation& Request);

    void CompleteRequest(uint64_t Id, int Result);

    static int GetSocketProtocol(int Pid, int Fd);

    std::map<PortAllocation, std::optional<time_t>> m_allocatedPorts;
    std::shared_ptr<wsl::shared::SocketChannel> m_hvSocketChannel;
    NetlinkChannel m_channel;
    std::promise<PortRefreshResult> m_allocatedPortsRefresh;

    WaitableValue<seccomp_notif> m_request;
    WaitableValue<int> m_reply;

    std::shared_ptr<SecCompDispatcher> m_seccompDispatcher;

    std::string m_networkNamespace;
};

std::ostream& operator<<(std::ostream& out, const GnsPortTracker::PortAllocation& portAllocation);
