// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9platform.h"
#include "p9io.h"

namespace p9fs {

class Socket final : public ISocket
{
public:
    Socket(int socket = -1);

    Task<std::unique_ptr<ISocket>> AcceptAsync(CancelToken& token) override;
    Task<size_t> RecvAsync(gsl::span<gsl::byte> buffer, CancelToken& token) override;
    Task<size_t> SendAsync(gsl::span<const gsl::byte> buffer, CancelToken& token) override;
    void Reset(int socket = -1);

private:
    wil::unique_fd m_Socket;
    CoroutineEpollIssuer m_Io;
};

class WorkItem final : public IWorkItem
{
public:
    WorkItem(std::function<void()> callback);

    void Submit() override;

private:
    std::function<void()> m_Callback;
};

class ThreadPool final
{
public:
    ThreadPool();

    void SubmitWork(std::function<void()> callback);

private:
    void WorkerCallback();

    std::queue<std::function<void()>> m_WorkQueue;
    std::mutex m_Lock;
    std::condition_variable m_Condition;
    unsigned int m_MaxThreads{};
    unsigned int m_AvailableThreads{};
    unsigned int m_RunningThreads{};
};

} // namespace p9fs
