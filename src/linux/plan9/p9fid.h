// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9defs.h"
#include "p9await.h"
#include "p9errors.h"
#include "p9protohelpers.h"
#include "p9handler.h"

namespace p9fs {

class XAttrBase;

class Fid
{
public:
    virtual ~Fid() = default;

    virtual Expected<Qid> Walk(std::string_view Name);
    virtual Expected<std::tuple<UINT64, Qid, StatResult>> GetAttr(UINT64 Mask);
    virtual LX_INT SetAttr(UINT32 Valid, const StatResult& Stat);
    virtual Expected<Qid> Open(OpenFlags Flags);
    virtual Expected<Qid> Create(std::string_view Name, OpenFlags Flags, UINT32 Mode, UINT32 Gid);
    virtual Expected<Qid> MkDir(std::string_view Name, UINT32 Mode, UINT32 Gid);
    virtual LX_INT ReadDir(UINT64 Offset, SpanWriter& Writer, bool IncludeAttributes);
    virtual Task<Expected<UINT32>> Read(UINT64 Offset, gsl::span<gsl::byte> Buffer);
    virtual Task<Expected<UINT32>> Write(UINT64 Offset, gsl::span<const gsl::byte> Buffer);
    virtual LX_INT UnlinkAt(std::string_view Name, UINT32 Flags);
    virtual LX_INT Remove();
    virtual LX_INT RenameAt(std::string_view OldName, Fid& NewParent, std::string_view NewName);
    virtual LX_INT Rename(Fid& NewParent, std::string_view NewName);
    virtual Expected<Qid> SymLink(std::string_view Name, std::string_view Target, UINT32 Gid);
    virtual Expected<Qid> MkNod(std::string_view Name, UINT32 Mode, UINT32 Major, UINT32 Minor, UINT32 Gid);
    virtual LX_INT Link(std::string_view Name, Fid& Target);
    virtual Expected<UINT32> ReadLink(gsl::span<char> Name);
    virtual LX_INT Fsync();
    virtual Expected<StatFsResult> StatFs();
    virtual Expected<LockStatus> Lock(LockType Type, UINT32 Flags, UINT64 Start, UINT64 Length, UINT32 ProcId, std::string_view ClientId);
    virtual Expected<std::tuple<LockType, UINT64, UINT64, UINT32, std::string_view>> GetLock(
        LockType Type, UINT64 Start, UINT64 Length, UINT32 ProcId, std::string_view ClientId);
    virtual Expected<std::shared_ptr<XAttrBase>> XattrWalk(const std::string& Name);
    virtual Expected<std::shared_ptr<XAttrBase>> XattrCreate(const std::string& Name, UINT64 Size, UINT32 Flags);
    virtual LX_INT Clunk();

    // 9P2000.W operations
    virtual LX_INT Access(AccessFlags Flags);

    virtual std::shared_ptr<Fid> Clone() const;
    virtual bool IsOnRoot(const std::shared_ptr<const IRoot>& root);
    virtual bool IsFile() const;
    virtual Qid GetQid() const;

protected:
    Fid() = default;
};

class XAttrBase : public Fid
{
public:
    virtual Expected<UINT64> GetSize() = 0;
};

#undef CreateFile
Expected<std::tuple<std::shared_ptr<Fid>, Qid>> CreateFile(std::shared_ptr<const IRoot> root, LX_UID_T uid);

} // namespace p9fs
