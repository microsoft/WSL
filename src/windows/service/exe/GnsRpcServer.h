// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "helpers.hpp"
#include <map>
#include <memory>
#include <wil/resource.h>

class GnsRpcServer
{
public:
    using AdapterCallback = std::function<HRESULT(LPCWSTR)>;

    GnsRpcServer();
    ~GnsRpcServer() noexcept;

    GnsRpcServer(const GnsRpcServer&) = delete;
    GnsRpcServer& operator=(const GnsRpcServer&) = delete;

    GnsRpcServer(GnsRpcServer&&) noexcept;
    GnsRpcServer& operator=(GnsRpcServer&&) noexcept;

    static std::shared_ptr<GnsRpcServer> GetOrCreate();

    const UUID& GetServerUuid() const noexcept;

private:
    wil::srwlock m_lock;
    bool m_moved = false;
    UUID m_serverUuid{};
};
