// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9await.h"

namespace p9fs {

// Platform-independent wrapper around socket operations.
class ISocket
{
public:
    virtual ~ISocket() = default;

    virtual Task<std::unique_ptr<ISocket>> AcceptAsync(CancelToken& token) = 0;
    virtual Task<size_t> RecvAsync(gsl::span<gsl::byte> buffer, CancelToken& token) = 0;
    virtual Task<size_t> SendAsync(gsl::span<const gsl::byte> buffer, CancelToken& token) = 0;
};

// Platform-independent wrapper around threadpool work
class IWorkItem
{
public:
    virtual ~IWorkItem() = default;

    virtual void Submit() = 0;
};

std::unique_ptr<IWorkItem> CreateWorkItem(std::function<void()> callback);

} // namespace p9fs
