// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

namespace p9fs {

// Interface for running the Plan 9 server.
// N.B. The main reason this is an interface, despite not needing COM like the Windows equivalent,
//      is so consumers can just include this header rather than needing most of the library's
//      headers and needing to compile with coroutine support, which would be required to directly
//      use the implementation of this interface.
class IPlan9FileSystem
{
public:
    virtual ~IPlan9FileSystem() noexcept = default;

    virtual void AddShare(const std::string& name, int rootFd) = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    virtual void Teardown() = 0;
    virtual bool HasConnections() const noexcept = 0;
};

std::unique_ptr<IPlan9FileSystem> CreateFileSystem(int socket);

} // namespace p9fs
