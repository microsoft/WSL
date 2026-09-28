// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9fid.h"

namespace p9fs {

class XAttr final : public XAttrBase
{
public:
    enum class Access
    {
        Read,
        Write
    };

    XAttr(const std::shared_ptr<const Root>& root, const std::string& fileName, const std::string& name, Access access, UINT64 size = 0, UINT32 flags = 0);

    Task<Expected<UINT32>> Read(UINT64 Offset, gsl::span<gsl::byte> Buffer) override;
    Task<Expected<UINT32>> Write(UINT64 Offset, gsl::span<const gsl::byte> Buffer) override;
    LX_INT Clunk() override;

    Expected<UINT64> GetSize() override;

private:
    Expected<UINT64> GetValue(gsl::span<gsl::byte> Buffer);

    std::shared_mutex m_Lock;
    const std::shared_ptr<const Root> m_Root;
    const std::string m_FileName;
    const std::string m_Name;
    std::vector<gsl::byte> m_Value;
    const Access m_Access;
    const UINT32 m_Flags;
};

} // namespace p9fs
