// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9errors.h"

namespace p9fs::util {

constexpr uid_t c_InvalidUid = std::numeric_limits<uid_t>::max();
constexpr gid_t c_InvalidGid = std::numeric_limits<gid_t>::max();

Expected<wil::unique_fd> Reopen(int fd, int openFlags);

Expected<wil::unique_fd> OpenAt(int dirfd, const std::string& name, int openFlags, mode_t mode = 0600);

std::string GetFdPath(int fd);

LX_INT AccessHelper(int fd, const std::string& path, int mode);

LX_INT CheckFOwnerCapability();

gid_t GetUserGroupId(uid_t uid);

gid_t GetGroupIdByName(const char* name);

// Changes the effective uid and gid of the current thread for the lifetime of this object.
class FsUserContext final
{
public:
    FsUserContext(uid_t uid, gid_t gid, const std::vector<gid_t>& groups);
    ~FsUserContext();

private:
    bool m_Restore{};
    bool m_restoreGroups{};
};

} // namespace p9fs::util
