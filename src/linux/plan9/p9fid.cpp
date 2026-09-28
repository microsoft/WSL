// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9fid.h"

namespace p9fs {

Expected<Qid> Fid::Walk(std::string_view)
{
    return LxError{LX_EINVAL};
}

Expected<std::tuple<UINT64, Qid, StatResult>> Fid::GetAttr(UINT64)
{
    return LxError{LX_EINVAL};
}

LX_INT Fid::SetAttr(UINT32, const StatResult&)
{
    return LX_EINVAL;
}

Expected<Qid> Fid::Open(OpenFlags)
{
    return LxError{LX_EINVAL};
}

Expected<Qid> Fid::Create(std::string_view, OpenFlags, UINT32, UINT32)
{
    return LxError{LX_EINVAL};
}

Expected<Qid> Fid::MkDir(std::string_view, UINT32, UINT32)
{
    return LxError{LX_EINVAL};
}

LX_INT Fid::ReadDir(UINT64, SpanWriter&, bool)
{
    return LX_EINVAL;
}

Task<Expected<UINT32>> Fid::Read(UINT64, gsl::span<gsl::byte>)
{
    co_return LxError{LX_EINVAL};
}

Task<Expected<UINT32>> Fid::Write(UINT64, gsl::span<const gsl::byte>)
{
    co_return LxError{LX_EINVAL};
}

LX_INT Fid::UnlinkAt(std::string_view, UINT32)
{
    return LX_EINVAL;
}

LX_INT Fid::Remove()
{
    return LX_EINVAL;
}

LX_INT Fid::RenameAt(std::string_view, Fid&, std::string_view)
{
    return LX_EINVAL;
}

LX_INT Fid::Rename(Fid&, std::string_view)
{
    return LX_EINVAL;
}

Expected<Qid> Fid::SymLink(std::string_view, std::string_view, UINT32)
{
    return LxError{LX_EINVAL};
}

Expected<Qid> Fid::MkNod(std::string_view, UINT32, UINT32, UINT32, UINT32)
{
    return LxError{LX_EINVAL};
}

LX_INT Fid::Link(std::string_view, Fid&)
{
    return LX_EINVAL;
}

Expected<UINT32> Fid::ReadLink(gsl::span<char>)
{
    return LxError{LX_EINVAL};
}

LX_INT Fid::Fsync()
{
    return LX_EINVAL;
}

Expected<StatFsResult> Fid::StatFs()
{
    return LxError{LX_EINVAL};
}

Expected<LockStatus> Fid::Lock(LockType, UINT32, UINT64, UINT64, UINT32, const std::string_view)
{
    return LxError{LX_EINVAL};
}

Expected<std::tuple<LockType, UINT64, UINT64, UINT32, std::string_view>> Fid::GetLock(LockType, UINT64, UINT64, UINT32, const std::string_view)
{
    return LxError{LX_EINVAL};
}

Expected<std::shared_ptr<XAttrBase>> Fid::XattrWalk(const std::string&)
{
    return LxError{LX_EINVAL};
}

Expected<std::shared_ptr<XAttrBase>> Fid::XattrCreate(const std::string&, UINT64, UINT32)
{
    return LxError{LX_EINVAL};
}

LX_INT Fid::Clunk()
{
    // The default implementation of Clunk returns success rather than error,
    // because all fid's must support clunk.
    return {};
}

LX_INT Fid::Access(AccessFlags)
{
    return LX_ENOTSUP;
}

std::shared_ptr<Fid> Fid::Clone() const
{
    THROW_INVALID();
}

bool Fid::IsOnRoot(const std::shared_ptr<const IRoot>&)
{
    THROW_INVALID();
}

bool Fid::IsFile() const
{
    return false;
}

Qid Fid::GetQid() const
{
    THROW_INVALID();
}

} // namespace p9fs
