/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    escape.h

Abstract:

    This file contains declarations for escaping Linux paths for us on NTFS using
    the DrvFs escape conventions.

--*/

void EscapePathForNt(const char* Path, char* EscapedPath);

size_t EscapePathForNtLength(const char* Path);

void UnescapePathInplace(char* Path);
