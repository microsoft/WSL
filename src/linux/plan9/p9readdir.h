// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

namespace p9fs {

class DirectoryEnumerator final
{
public:
    DirectoryEnumerator(int fd);
    ~DirectoryEnumerator();

    struct dirent* Next();
    void Seek(long offset);
    int Fd();

private:
    DIR* m_Dir{};
    long m_LastOffset{};
};

} // namespace p9fs
