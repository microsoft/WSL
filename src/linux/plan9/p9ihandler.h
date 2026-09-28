// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

namespace p9fs {

// This class serves as a base for the platform-specific Root class. It has
// no functionality.
class IRoot
{
public:
    virtual ~IRoot() = default;

protected:
    IRoot() = default;
};

class IShareList
{
public:
    virtual ~IShareList() = default;

    virtual Expected<std::shared_ptr<const IRoot>> MakeRoot(std::string_view aname, LX_UID_T uid) = 0;
    virtual size_t MaximumConnectionCount() = 0;
};

// Interface through which virtio can process messages on a handler.
// N.B. This definition is not in p9handler so it can be used without needing to include all the
//      coroutine related headers as well.
class IHandler
{
public:
    using HandlerCallback = std::function<void(const std::vector<gsl::byte>& response)>;
    virtual void ProcessMessageAsync(std::vector<gsl::byte>&& message, size_t responseSize, HandlerCallback&& callback) = 0;
};

class HandlerFactory
{
public:
    HandlerFactory(IShareList& shareList) : m_shareList{shareList}
    {
    }

    std::unique_ptr<IHandler> CreateHandler() const;

private:
    IShareList& m_shareList;
};

} // namespace p9fs
