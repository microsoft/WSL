// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9io.h"
#include "p9fid.h"
#include "p9readdir.h"
#include <pwd.h>
#include <grp.h>

namespace p9fs {

struct Share
{
    wil::unique_fd RootFd;
};

struct Root final : public IRoot
{
    Root(std::shared_ptr<const Share> share, int rootFd, uid_t uid, gid_t gid) : Share{share}, RootFd{rootFd}, Uid{uid}, Gid{gid}
    {
        Plan9TraceLoggingProvider::LogMessage(std::format("Instantiate root, uid={}", uid));
        if (uid == -1)
        {
            return; // No uid passed, don't try to get the additional groups.
        }

        auto bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufsize == -1)
        {
            bufsize = 16384; // Recommended by the man page if _SC_GETPW_R_SIZE_MAX is not set.
        }

        std::vector<char> buffer(bufsize);
        passwd pwd{};
        passwd* result = nullptr;
        if (getpwuid_r(uid, &pwd, buffer.data(), buffer.size(), &result) < 0 || result == nullptr)
        {
            Plan9TraceLoggingProvider::LogMessage(std::format("getpwuid_r failed for uid: {}, errno={}", uid, errno));
            return;
        }

        // Find the number of groups
        int groupCount = 0;
        getgrouplist(pwd.pw_name, gid, nullptr, &groupCount);
        Groups.resize(groupCount);

        // Query the groups
        if (getgrouplist(pwd.pw_name, gid, Groups.data(), &groupCount) < 0)
        {
            Plan9TraceLoggingProvider::LogMessage(std::format("getgrouplist failed for user: {}, errno={}", pwd.pw_name, errno));
            Groups.clear();
        }
    }

    std::shared_ptr<const Share> Share;

    int RootFd;

    // The uid that the client attached with, and the associated primary gid.
    // If these are -1, then no change is necessary.
    uid_t Uid;
    gid_t Gid;
    std::vector<gid_t> Groups;

    bool ReadOnly() const
    {
        return false;
    }
};

class File final : public Fid
{
public:
    File(std::shared_ptr<const Root> root);
    File(const File&);

    Expected<Qid> Initialize();
    Expected<Qid> Walk(std::string_view Name) override;
    Expected<std::tuple<UINT64, Qid, StatResult>> GetAttr(UINT64 Mask) override;
    LX_INT SetAttr(UINT32 Valid, const StatResult& Stat) override;
    Expected<Qid> Open(OpenFlags Flags) override;
    Expected<Qid> Create(std::string_view Name, OpenFlags /* Flags */, UINT32 /* Mode */, UINT32 /* Gid */) override;
    Expected<Qid> MkDir(std::string_view Name, UINT32 /* Mode */, UINT32 /* Gid */) override;
    LX_INT ReadDir(UINT64 Offset, SpanWriter& writer, bool includeAttributes) override;
    Task<Expected<UINT32>> Read(UINT64 Offset, gsl::span<gsl::byte> Buffer) override;
    Task<Expected<UINT32>> Write(UINT64 Offset, gsl::span<const gsl::byte> Buffer) override;
    LX_INT UnlinkAt(std::string_view Name, UINT32 /* Flags */) override;
    LX_INT Remove() override;
    LX_INT RenameAt(std::string_view OldName, Fid& NewParent, std::string_view NewName) override;
    LX_INT Rename(Fid& NewParent, std::string_view NewName) override;
    Expected<Qid> SymLink(std::string_view /* name */, std::string_view /* target */, UINT32 /* gid */) override;
    Expected<Qid> MkNod(std::string_view /* name */, UINT32 /* mode */, UINT32 /* major */, UINT32 /* minor */, UINT32 /* gid */) override;
    LX_INT Link(std::string_view /* name */, Fid& /* target */) override;
    Expected<UINT32> ReadLink(gsl::span<char> /* name */) override;
    LX_INT Fsync() override;
    Expected<StatFsResult> StatFs() override;
    Expected<LockStatus> Lock(LockType Type, UINT32 Flags, UINT64 Start, UINT64 Length, UINT32 ProcId, std::string_view ClientId) override;
    Expected<std::tuple<LockType, UINT64, UINT64, UINT32, std::string_view>> GetLock(
        LockType Type, UINT64 Start, UINT64 Length, UINT32 ProcId, std::string_view ClientId) override;
    Expected<std::shared_ptr<XAttrBase>> XattrWalk(const std::string& Name) override;
    Expected<std::shared_ptr<XAttrBase>> XattrCreate(const std::string& Name, UINT64 Size, UINT32 Flags) override;

    // 9P2000.W operations
    LX_INT Access(AccessFlags Flags) override;

    std::shared_ptr<Fid> Clone() const override;
    bool IsOnRoot(const std::shared_ptr<const IRoot>& root) override;
    bool IsFile() const override;
    Qid GetQid() const override;
    bool IsOpen() const;

private:
    Expected<wil::unique_fd> OpenFile(int openFlags);
    LX_INT ValidateExists();
    std::string GetFileName() const;
    std::string ChildPath(std::string_view name);
    std::string ChildPathWithLockHeld(std::string_view name);
    Expected<struct stat> Stat();
    LX_INT ReadDirHelper(UINT64 offset, SpanWriter& writer, bool extendedAttributes);

    // This lock protects all state except:
    // - Read access to m_File: once non-NULL, this member never becomes NULL
    //   again.
    // - m_Root, m_Uid: these members don't change after initialization.
    mutable std::shared_mutex m_Lock;
    std::string m_FileName;
    std::unique_ptr<DirectoryEnumerator> m_Enumerator;
    wil::unique_fd m_File;
    CoroutineIoIssuer m_Io;
    const std::shared_ptr<const Root> m_Root;
    Qid m_Qid{};
    dev_t m_Device{};
};
} // namespace p9fs
