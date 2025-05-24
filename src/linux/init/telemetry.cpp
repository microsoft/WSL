/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    telemetry.cpp

Abstract:

    This file contains the telemetry agent implementation.

--*/

#include "common.h"
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <linux/cn_proc.h>
#include <string>
#include <lxwil.h>
#include "util.h"
#include "mountutil.h"
#include "SocketChannel.h"
#include "message.h"

namespace {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"

union messageBuffer
{
    struct
    {
        nlmsghdr netlinkHeader;
        cn_msg connectorMessage;
        proc_cn_mcast_op operation;
    } Send;

    struct
    {
        nlmsghdr netlinkHeader;
        cn_msg connectorMessage;
        proc_event event;
    } Receive;
};

#pragma GCC diagnostic pop

// Map containing the binaries and commands that are filesystem-intensive that we want to warn users against using in DrvFs.
const std::map<std::string, std::string> g_drvFsUsageMap = {
    {"git", "clone"}, {"node", "/usr/bin/npm install"}, {"cargo", "build"}};

bool g_drvFsUsageEnabled = true;

} // namespace

std::pair<std::string, bool> GetProcessInformation(int pid)
{
    // N.B. Procfs files may no longer be present for short-lived processes that exit before the
    //      process creation notification can be processed.
    const std::string procPidPath = std::format("/proc/{}", pid);
    wil::unique_fd fd{open(std::format("{}/cmdline", procPidPath).c_str(), O_RDONLY)};
    if (!fd)
    {
        return {};
    }

    // /proc/pid/cmdline contains all the arguments separated by NULL characters.
    LX_MINI_INIT_TELEMETRY_MESSAGE message{};
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = LX_MINI_INIT_TELEMETRY_MESSAGE::Type;

    std::string commandLine(256, '\0');
    auto bytesRead = TEMP_FAILURE_RETRY(read(fd.get(), commandLine.data(), commandLine.size()));
    if (bytesRead <= 0)
    {
        return {};
    }

    commandLine.resize(bytesRead - 1);
    const auto* executable = basename(commandLine.data());

    bool showDrvfsNotification = false;

    // Determine if the DrvFs perf notification should be displayed.
    if (g_drvFsUsageEnabled)
    {
        // Check the if the binary name and first argument are in the list of scenarios.
        auto found = g_drvFsUsageMap.find(executable);
        if (found != g_drvFsUsageMap.end())
        {
            auto length = strlen(executable);
            if (bytesRead > length + 1)
            {
                auto argument = std::string_view(commandLine.data(), bytesRead).substr(length + 1);
                if (strcmp(argument.data(), found->second.c_str()) == 0)
                {
                    // Determine if the current working directory is a DrvFs mount.
                    std::error_code errorCode;
                    auto cwd = std::filesystem::read_symlink(std::format("{}/cwd", procPidPath), errorCode);
                    if (!errorCode)
                    {
                        auto mountInfo = std::format("{}{}", procPidPath, MOUNT_INFO_FILE_NAME);
                        size_t prefixLength;
                        auto drvFsPrefix = UtilFindMount(mountInfo.c_str(), cwd.c_str(), false, &prefixLength);
                        if (!drvFsPrefix.empty())
                        {
                            showDrvfsNotification = true;
                            g_drvFsUsageEnabled = false;
                        }
                    }
                }
            }
        }
    }

    return std::make_pair(executable, showDrvfsNotification);
}

unsigned int StartTelemetryAgent()
{
    try
    {
        constexpr auto flushPeriod = std::chrono::minutes(30);

        // The telemetry agent is only supported on VM mode.
        if (!UtilIsUtilityVm())
        {
            return 1;
        }

        // Initialize logging.
        InitializeLogging(false);

        // Open and bind a netlink socket.
        wil::unique_fd fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
        THROW_LAST_ERROR_IF(!fd);

        sockaddr_nl address{};
        address.nl_family = AF_NETLINK;
        address.nl_groups = CN_IDX_PROC;
        address.nl_pid = getpid();
        THROW_LAST_ERROR_IF(bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0);

        // Fill in the netlink header and connector message and send a message.
        messageBuffer buffer{};
        buffer.Send.netlinkHeader.nlmsg_len = sizeof(buffer.Send);
        buffer.Send.netlinkHeader.nlmsg_type = NLMSG_DONE;
        buffer.Send.netlinkHeader.nlmsg_pid = getpid();
        buffer.Send.connectorMessage.id.idx = CN_IDX_PROC;
        buffer.Send.connectorMessage.id.val = CN_VAL_PROC;
        buffer.Send.connectorMessage.len = sizeof(buffer.Send.operation);
        buffer.Send.operation = proc_cn_mcast_op::PROC_CN_MCAST_LISTEN;
        auto bytes = send(fd.get(), &buffer, sizeof(buffer.Send), 0);
        THROW_LAST_ERROR_IF(bytes != sizeof(buffer.Send));

        // Set the receive timeout to 1 minute so the thread has an opportunity to flush even when no events are received.
        timeval tv{};
        tv.tv_sec = 10;
        THROW_LAST_ERROR_IF(setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0);

        wsl::shared::SocketChannel channel({STDOUT_FILENO}, "Telemetry");

        std::map<std::string, size_t> events;
        std::optional<std::string> drvfsNotifyCommand;

        // Schedule the next flush in 30 seconds so that some events are captured even if WSL shuts down quickly.
        auto nextFlush = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        // Begin reading netlink messages.
        for (;;)
        {
            memset(&buffer, 0, sizeof(buffer));
            sockaddr_nl fromAddress{};
            fromAddress.nl_family = AF_NETLINK;
            fromAddress.nl_groups = CN_IDX_PROC;
            fromAddress.nl_pid = 1;
            socklen_t addressLength = sizeof(fromAddress);
            bytes = TEMP_FAILURE_RETRY(recvfrom(fd.get(), &buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&fromAddress), &addressLength));

            if (bytes <= 0)
            {
                THROW_LAST_ERROR_IF(errno != ETIMEDOUT && errno != EAGAIN);
            }
            else
            {
                for (nlmsghdr* netlinkHeader = &buffer.Receive.netlinkHeader; NLMSG_OK(netlinkHeader, bytes);
                     netlinkHeader = NLMSG_NEXT(netlinkHeader, bytes))
                {
                    if ((netlinkHeader->nlmsg_type == NLMSG_ERROR) || (netlinkHeader->nlmsg_type == NLMSG_OVERRUN))
                    {
                        break;
                    }

                    if (netlinkHeader->nlmsg_type == NLMSG_NOOP)
                    {
                        continue;
                    }

                    // For exec events, log app usage telemetry.
                    auto event = reinterpret_cast<proc_event*>(((cn_msg*)NLMSG_DATA(netlinkHeader))->data);
                    if (event->what == PROC_EVENT_EXEC)
                    {
                        auto [executable, showNotification] = GetProcessInformation(event->event_data.exec.process_pid);
                        if (showNotification)
                        {
                            drvfsNotifyCommand = executable;
                        }

                        // Make sure the name doesn't contain a '/' so it doesn't break our message format.
                        if (!executable.empty() && executable.find("/") == std::string::npos)
                        {
                            auto it = events.find(executable);
                            if (it == events.end())
                            {
                                events.emplace(std::move(executable), 1);
                            }
                            else
                            {
                                it->second++;
                            }
                        }
                    }

                    if (netlinkHeader->nlmsg_type == NLMSG_DONE)
                    {
                        break;
                    }
                }
            }

            // Regularly flush messages back to the service.

            auto now = std::chrono::steady_clock::now();
            if (drvfsNotifyCommand.has_value() || now > nextFlush)
            {
                if (!events.empty())
                {
                    std::stringstream content;

                    if (drvfsNotifyCommand.has_value())
                    {
                        content << drvfsNotifyCommand.value() << "/1/";
                    }

                    for (auto [executable, count] : events)
                    {
                        // Having an extra ',' at the end makes parsing simpler.
                        content << executable << '/' << count << '/';
                    }

                    wsl::shared::MessageWriter<LX_MINI_INIT_TELEMETRY_MESSAGE> message;
                    message->ShowDrvFsNotification = drvfsNotifyCommand.has_value();
                    message.WriteString(content.str());

                    channel.SendMessage<LX_MINI_INIT_TELEMETRY_MESSAGE>(message.Span());

                    events.clear();
                    drvfsNotifyCommand = {};
                }

                nextFlush = now + flushPeriod;
            }
        }

        return 0;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return 1;
    }

    return 0;
}