/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslpath.h

Abstract:

    This file contains wslpath function declarations.

--*/

#pragma once

#define WSLPATH_NAME "wslpath"

#define TRANSLATE_FLAG_ABSOLUTE (0x1)
#define TRANSLATE_FLAG_RESOLVE_SYMLINKS (0x2)

#define TRANSLATE_MODE_ABSOLUTE 'a'
#define TRANSLATE_MODE_UNIX 'u'
#define TRANSLATE_MODE_WINDOWS 'w'
#define TRANSLATE_MODE_MIXED 'm'
#define TRANSLATE_MODE_HELP 'h'

int WslPathEntry(int Argc, char* Argv[]);

std::string WslPathTranslate(char* Path, int Flags, char Mode);
