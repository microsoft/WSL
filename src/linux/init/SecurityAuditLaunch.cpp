// Copyright (C) Microsoft Corporation. All rights reserved.

#include "SecurityAuditLaunch.h"
#include "common.h"
#include "util.h"
#include "Launch.h"
#include "SocketChannel.h"
#include "stringshared.h"

#include <linux/vm_sockets.h>

namespace {

constexpr unsigned int c_maxLaunchMessageSize = 64 * 1024;
constexpr timeval c_requestTimeout{5, 0};

void ServeRequest(wil::unique_fd socket)
{
    THROW_LAST_ERROR_IF(setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &c_requestTimeout, sizeof(c_requestTimeout)) < 0);
    THROW_LAST_ERROR_IF(setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &c_requestTimeout, sizeof(c_requestTimeout)) < 0);
    // Hyper-V sockets cannot peek. Check the size in the shared decoder before allocating the message body.
    wsl::shared::SocketChannel channel{std::move(socket), "SecurityAuditLaunch", c_maxLaunchMessageSize};
    // Linux transactions do not implement receive timeouts; use the socket's bounded I/O above.
    auto transaction = channel.ReceiveTransaction();
    gsl::span<gsl::byte> buffer;
    const auto& request = transaction.Receive<CREATE_PROCESS_MESSAGE>(&buffer);
    const auto* path = wsl::shared::string::FromSpan(buffer, request.PathIndex);
    THROW_ERRNO_IF(EPERM, strcmp(path, wsl::audit::c_nativeAuditCollectorPath) != 0);
    const auto arguments = wsl::shared::string::ArrayFromSpan(buffer, request.CommandLineIndex);
    THROW_ERRNO_IF(EINVAL, arguments.empty() || arguments.size() > 16);

    THROW_LAST_ERROR_IF(ProcessCreateProcessMessage(transaction, buffer, std::nullopt, -1, true) < 0);
}

void RunListener(int listener)
{
    // A cancelled host request must fail its write with EPIPE, not terminate this VM-lifetime worker.
    THROW_LAST_ERROR_IF(IgnoreSecurityAuditListenerSigpipe() < 0);

    // This worker is created before the host control channels. Its children do not need waitpid bookkeeping.
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    THROW_LAST_ERROR_IF(sigemptyset(&action.sa_mask) < 0);
    THROW_LAST_ERROR_IF(sigaction(SIGCHLD, &action, nullptr) < 0);
    sigset_t mask{};
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    THROW_LAST_ERROR_IF(sigprocmask(SIG_UNBLOCK, &mask, nullptr) < 0);
    THROW_LAST_ERROR_IF(UtilMkdir(RUN_FOLDER, 0755) < 0);

    for (;;)
    {
        sockaddr_vm peer{};
        socklen_t peerSize = sizeof(peer);
        wil::unique_fd connection{
            static_cast<int>(TEMP_FAILURE_RETRY(accept4(listener, reinterpret_cast<sockaddr*>(&peer), &peerSize, SOCK_CLOEXEC)))};
        if (!connection)
        {
            LOG_ERROR("Security audit launch accept failed: {}", errno);
            return;
        }
        if (peerSize != sizeof(peer) || peer.svm_family != AF_VSOCK || peer.svm_cid != VMADDR_CID_HOST)
        {
            LOG_ERROR("Rejected non-host security audit launch connection");
            continue;
        }
        try
        {
            ServeRequest(std::move(connection));
        }
        CATCH_LOG()
    }
}

} // namespace

void StartSecurityAuditListener() noexcept
{
    try
    {
        wil::unique_fd listener{socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        THROW_LAST_ERROR_IF(!listener);
        sockaddr_vm address{};
        address.svm_family = AF_VSOCK;
        address.svm_cid = VMADDR_CID_ANY;
        address.svm_port = wsl::audit::c_nativeAuditLaunchPort;
        THROW_LAST_ERROR_IF(bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0);
        THROW_LAST_ERROR_IF(listen(listener.get(), 4) < 0);
        THROW_LAST_ERROR_IF(UtilCreateChildProcess("audit-launch", [&]() { RunListener(listener.get()); }) < 0);
    }
    CATCH_LOG()
}
