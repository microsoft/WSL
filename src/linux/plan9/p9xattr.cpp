// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9file.h"
#include "p9xattr.h"
#include "p9util.h"

namespace p9fs {

XAttr::XAttr(const std::shared_ptr<const Root>& root, const std::string& fileName, const std::string& name, Access access, UINT64 size, UINT32 flags) :
    m_Root{root}, m_FileName{fileName}, m_Name{name}, m_Value{size}, m_Access{access}, m_Flags{flags}
{
}

Task<Expected<UINT32>> XAttr::Read(UINT64 offset, gsl::span<gsl::byte> buffer)
{
    // In practice, the Linux Plan 9 client never uses a non-zero offset, so
    // it's not implemented here (otherwise an intermediate buffer would be
    // needed).
    if ((m_Access != Access::Read) || (offset != 0))
    {
        co_return LxError{LX_EINVAL};
    }

    auto result = GetValue(buffer);
    if (!result)
    {
        co_return result.Unexpected();
    }

    co_return static_cast<UINT32>(result.Get());
}

Task<Expected<UINT32>> XAttr::Write(UINT64 offset, gsl::span<const gsl::byte> buffer)
{
    if (m_Access != Access::Write)
    {
        co_return LxError{LX_EINVAL};
    }

    if (offset > m_Value.size())
    {
        co_return 0;
    }

    std::lock_guard<std::shared_mutex> lock{m_Lock};
    UINT32 length = gsl::narrow_cast<UINT32>(buffer.size());
    if (length > m_Value.size() - offset)
    {
        length = m_Value.size() - offset;
    }

    if (length > 0)
    {
        gsl::copy(buffer.subspan(0, length), gsl::make_span(m_Value).subspan(offset));
    }

    co_return UINT32{length};
}

LX_INT XAttr::Clunk()
{
    // Nothing to do is this fid is not for write.
    if (m_Access != Access::Write)
    {
        return {};
    }

    // Make sure in-flight write operations are finished.
    std::shared_lock<std::shared_mutex> lock{m_Lock};

    // Remove the xattr if its size is 0; otherwise, set the value.
    // N.B. Plan 9 does not support xattrs with zero-length values.
    if (m_Value.size() == 0)
    {
        const int result = lremovexattr(m_FileName.c_str(), m_Name.c_str());
        if (result < 0)
        {
            return -errno;
        }
    }
    else
    {
        int result = lsetxattr(m_FileName.c_str(), m_Name.c_str(), m_Value.data(), m_Value.size(), m_Flags);
        if (result < 0)
        {
            return -errno;
        }
    }

    return {};
}

Expected<UINT64> XAttr::GetSize()
{
    return GetValue({});
}

Expected<UINT64> XAttr::GetValue(gsl::span<gsl::byte> buffer)
{
    util::FsUserContext userContext{m_Root->Uid, m_Root->Gid, m_Root->Groups};
    ssize_t result;
    if (m_Name.size() > 0)
    {
        result = lgetxattr(m_FileName.c_str(), m_Name.c_str(), buffer.data(), buffer.size());
        if (result < 0)
        {
            return LxError{-errno};
        }
    }
    else
    {
        result = llistxattr(m_FileName.c_str(), reinterpret_cast<char*>(buffer.data()), buffer.size());
        if (result < 0)
        {
            return LxError{-errno};
        }
    }

    return static_cast<UINT64>(result);
}

} // namespace p9fs
