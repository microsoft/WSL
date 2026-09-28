// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9errors.h"
#include "p9handler.h"
#include "p9file.h"
#include "p9fs.h"
#include "p9lx.h"
#include "p9util.h"
#include "p9tracelogging.h"

namespace p9fs {

constexpr const char* c_NobodyGroupName = "nobody";

class ShareList final : public IShareList
{
public:
    void Add(const std::string& name, int rootFd);
    void Remove(const std::string& name);
    std::shared_ptr<const Share> Get(std::string_view name);
    size_t MaximumConnectionCount() override;
    Expected<std::shared_ptr<const IRoot>> MakeRoot(std::string_view aname, LX_UID_T uid) override;

private:
    std::mutex m_ShareLock;
    std::map<std::string, std::shared_ptr<Share>, std::less<>> m_Shares;
};

void ShareList::Add(const std::string& name, int rootFd)
{
    auto share = std::make_shared<Share>();
    share->RootFd.reset(rootFd);
    THROW_LAST_ERROR_IF(!share->RootFd);

    std::lock_guard<std::mutex> lock{m_ShareLock};
    const bool inserted = m_Shares.try_emplace(name, std::move(share)).second;
    if (!inserted)
    {
        THROW_ERRNO(EEXIST);
    }
}

void ShareList::Remove(const std::string& name)
{
    std::lock_guard<std::mutex> lock{m_ShareLock};
    const auto share = m_Shares.find(name);
    if (share == m_Shares.end())
    {
        THROW_ERRNO(ENOENT);
    }

    m_Shares.erase(share);
}

std::shared_ptr<const Share> ShareList::Get(std::string_view name)
{
    std::lock_guard<std::mutex> lock{m_ShareLock};
    auto it = m_Shares.find(name);
    if (it != m_Shares.end())
    {
        return it->second;
    }

    return {};
}

// Returns the maximum number of concurrent connections that should be allowed
// based on the number and configuration of the shares.
size_t ShareList::MaximumConnectionCount()
{
    return 4096;
}

Expected<std::shared_ptr<const IRoot>> ShareList::MakeRoot(std::string_view aname, LX_UID_T uid)
{
    auto share = Get(aname);
    if (!share)
    {
        return LxError{LX_ENOENT};
    }

    gid_t gid;
    uid_t currentUid = geteuid();
    if (uid == currentUid)
    {
        // No need to change IDs if the requested user matches the user the server is running as.
        uid = util::c_InvalidUid;
        gid = util::c_InvalidGid;
    }
    else if (currentUid == 0)
    {
        gid = util::GetUserGroupId(uid);
        if (gid == util::c_InvalidGid)
        {
            // The user wasn't found in /etc/passwd, so use "nobody" as the group.
            gid = util::GetGroupIdByName(c_NobodyGroupName);
            if (gid == util::c_InvalidGid)
            {
                // No group named nobody, so fail the connection.
                return LxError{LX_EINVAL};
            }
        }
    }
    else
    {
        // The server is not running as root, which won't work.
        // N.B. It's possible to make this work as long as the server has CAP_SETUID, but that
        //      is currently not needed.
        return LxError{LX_EPERM};
    }

    std::shared_ptr<const IRoot> root = std::make_shared<const Root>(share, share->RootFd.get(), uid, gid);
    return root;
}

class FileSystem final : public IPlan9FileSystem
{
public:
    // Creates a new file system, using the specified socket to listen.
    // N.B. The socket must already be bound to an appropriate local address.
    // N.B. The file system class takes ownership of the socket.
    FileSystem(int socket)
    {
        if (!g_Watcher)
        {
            g_Watcher.Run();
        }

        m_Server.Reset(socket);
        THROW_LAST_ERROR_IF(listen(socket, 1) < 0);
    }

    // Destructs a file system instance.
    ~FileSystem() noexcept override
    try
    {
        // Make sure the task finishes.
        Pause();
    }
    CATCH_LOG()

    // Add a share to the file system.
    // N.B. The root FD is duplicated so this function does not take ownership of it.
    void AddShare(const std::string& name, int rootFd) override
    {
        m_ShareList.Add(name, rootFd);
    }

    // Cancels any outstanding operations and stops listening for new connections.
    void Pause() override
    {
        if (m_RunTask)
        {
            Plan9TraceLoggingProvider::ServerStop();
            m_CancelToken.Cancel();
            try
            {
                m_RunTask.Get();
            }
            catch (...)
            {
                auto error = util::LinuxErrorFromCaughtException();
                if (error != LX_ECANCELED)
                {
                    LOG_CAUGHT_EXCEPTION();
                }
            }

            m_CancelToken.Reset();
            m_RunTask = {};
        }
    }

    // Runs the file system.
    void Resume() override
    {
        Plan9TraceLoggingProvider::ServerStart();
        m_RunTask = Run();
    }

    // Tears down the server socket.
    void Teardown() override
    {
        m_Server.Reset();
    }

    bool HasConnections() const noexcept override
    {
        return m_WaitGroup.HasMembers();
    }

private:
    // Asynchronously handles incoming connections.
    AsyncTask Run() noexcept
    {
        return HandleConnections(m_Server, m_ShareList, m_CancelToken, m_WaitGroup);
    }

    Socket m_Server;
    AsyncTask m_RunTask;
    CancelToken m_CancelToken;
    WaitGroup m_WaitGroup;
    ShareList m_ShareList;
};

std::unique_ptr<IPlan9FileSystem> CreateFileSystem(int socket)
{
    return std::make_unique<FileSystem>(socket);
}

} // namespace p9fs
