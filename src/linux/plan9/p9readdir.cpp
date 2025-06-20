// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "p9readdir.h"

namespace p9fs {

// Creates a new directory enumerator.
// N.B. If successful, this takes ownership of the specified fd.
DirectoryEnumerator::DirectoryEnumerator(int fd) : m_Dir{fdopendir(fd)}
{
    THROW_LAST_ERROR_IF(m_Dir == nullptr);
}

// Destructs the directory enumerator, closing the directory object and fd.
DirectoryEnumerator::~DirectoryEnumerator()
{
    if (m_Dir != nullptr)
    {
        closedir(m_Dir);
    }
}

struct dirent* DirectoryEnumerator::Next()
{
    errno = 0;
    auto result = readdir(m_Dir);
    if (result == nullptr)
    {
        // If errno is still 0, it means EOF is reached which is not an error.
        THROW_LAST_ERROR_IF(errno != 0)
    }
    else
    {
        m_LastOffset = result->d_off;
    }

    return result;
}

void DirectoryEnumerator::Seek(long offset)
{
    // If the offset hasn't changed, continue enumeration and avoid having to
    // refill the buffer.
    if (offset != m_LastOffset)
    {
        if (offset == 0)
        {
            rewinddir(m_Dir);
        }
        else
        {
            seekdir(m_Dir, offset);
        }

        m_LastOffset = offset;
    }
}

int DirectoryEnumerator::Fd()
{
    int fd = dirfd(m_Dir);
    THROW_LAST_ERROR_IF(fd < 0);
    return fd;
}

} // namespace p9fs
