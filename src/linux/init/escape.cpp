/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    escape.c

Abstract:

    This file contains support for escaping Linux paths for us on NTFS using
    the DrvFs escape conventions.

--*/

#include "common.h"
#include "escape.h"
#include "util.h"

//
// List indicating which characters are legal in NTFS.
// This differs from the Windows logic in two ways:
// 1. Slashes are allowed (because escaping is done on full Linux paths).
// 2. Colons are disallowed (because they indicate alternate data streams).
//

static constexpr bool EscapeNtfsLegalAnsiCharacterArray[128] = {
    false, // 0x00 ^@
    false, // 0x01 ^A
    false, // 0x02 ^B
    false, // 0x03 ^C
    false, // 0x04 ^D
    false, // 0x05 ^E
    false, // 0x06 ^F
    false, // 0x07 ^G
    false, // 0x08 ^H
    false, // 0x09 ^I
    false, // 0x0A ^J
    false, // 0x0B ^K
    false, // 0x0C ^L
    false, // 0x0D ^M
    false, // 0x0E ^N
    false, // 0x0F ^O
    false, // 0x10 ^P
    false, // 0x11 ^Q
    false, // 0x12 ^R
    false, // 0x13 ^S
    false, // 0x14 ^T
    false, // 0x15 ^U
    false, // 0x16 ^V
    false, // 0x17 ^W
    false, // 0x18 ^X
    false, // 0x19 ^Y
    false, // 0x1A ^Z
    false, // 0x1B ESC
    false, // 0x1C FS
    false, // 0x1D GS
    false, // 0x1E RS
    false, // 0x1F US
    true,  // 0x20 space
    true,  // 0x21 !
    false, // 0x22 "
    true,  // 0x23 #
    true,  // 0x24 $
    true,  // 0x25 %
    true,  // 0x26 &
    true,  // 0x27 '
    true,  // 0x28 (
    true,  // 0x29 )
    false, // 0x2A *
    true,  // 0x2B +
    true,  // 0x2C,
    true,  // 0x2D -
    true,  // 0x2E .
    true,  // 0x2F /   *** Normally "false"
    true,  // 0x30 0
    true,  // 0x31 1
    true,  // 0x32 2
    true,  // 0x33 3
    true,  // 0x34 4
    true,  // 0x35 5
    true,  // 0x36 6
    true,  // 0x37 7
    true,  // 0x38 8
    true,  // 0x39 9
    false, // 0x3A :   *** Normally "true"
    true,  // 0x3B ;
    false, // 0x3C <
    true,  // 0x3D =
    false, // 0x3E >
    false, // 0x3F ?
    true,  // 0x40 @
    true,  // 0x41 A
    true,  // 0x42 B
    true,  // 0x43 C
    true,  // 0x44 D
    true,  // 0x45 E
    true,  // 0x46 F
    true,  // 0x47 G
    true,  // 0x48 H
    true,  // 0x49 I
    true,  // 0x4A J
    true,  // 0x4B K
    true,  // 0x4C L
    true,  // 0x4D M
    true,  // 0x4E N
    true,  // 0x4F O
    true,  // 0x50 P
    true,  // 0x51 Q
    true,  // 0x52 R
    true,  // 0x53 S
    true,  // 0x54 T
    true,  // 0x55 U
    true,  // 0x56 V
    true,  // 0x57 W
    true,  // 0x58 X
    true,  // 0x59 Y
    true,  // 0x5A Z
    true,  // 0x5B [
    false, // 0x5C backslash
    true,  // 0x5D ]
    true,  // 0x5E ^
    true,  // 0x5F _
    true,  // 0x60 `
    true,  // 0x61 a
    true,  // 0x62 b
    true,  // 0x63 c
    true,  // 0x64 d
    true,  // 0x65 e
    true,  // 0x66 f
    true,  // 0x67 g
    true,  // 0x68 h
    true,  // 0x69 i
    true,  // 0x6A j
    true,  // 0x6B k
    true,  // 0x6C l
    true,  // 0x6D m
    true,  // 0x6E n
    true,  // 0x6F o
    true,  // 0x70 p
    true,  // 0x71 q
    true,  // 0x72 r
    true,  // 0x73 s
    true,  // 0x74 t
    true,  // 0x75 u
    true,  // 0x76 v
    true,  // 0x77 w
    true,  // 0x78 x
    true,  // 0x79 y
    true,  // 0x7A z
    true,  // 0x7B {
    false, // 0x7C |
    true,  // 0x7D }
    true,  // 0x7E ~
    true   // 0x7F 
};

//
// This is the utf-8 sequence for character 0xf000, the first character in the
// range used to escape unsupported characters.
//

static const char UtilEscapeCharBase[] = {0xef, 0x80, 0x80};

bool EscapeCharNeedsEscape(char Character)

/*++

Description:

    This routine checks whether a character needs to be escaped to be used in
    a path.

    N.B. Slashes are allowed because this function is used on complete Linux
         paths. The caller should translate those to backslashes after
         calling this function.

Parameters:

    Character - Supplies the character.

Return:

    True if the character needs to be escaped; otherwise, false.

--*/

{
    //
    // Check if the character needs to be escaped.
    //

    return (static_cast<unsigned char>(Character) <= SCHAR_MAX) && (EscapeNtfsLegalAnsiCharacterArray[static_cast<int>(Character)] == false);
}

void EscapePathForNt(const char* Path, char* EscapedPath)

/*++

Description:

    This routine escapes a Linux path for use with NT.

    N.B. The path is assumed to use Linux separators (forward slash), so those
         are not escaped, but rather replaced with backslashes.

Parameters:

    Path - Supplies the path to escape.

    EscapedPath - Supplies a buffer that receives the escaped path. This
        buffer is assumed to be the right length.

Return:

    None.

--*/

{
    const char* Current;
    size_t InsertionIndex;

    InsertionIndex = 0;
    for (Current = Path; *Current != '\0'; Current += 1)
    {
        if (*Current == PATH_SEP)
        {
            EscapedPath[InsertionIndex] = PATH_SEP_NT;
            InsertionIndex += 1;
        }
        else if (EscapeCharNeedsEscape(*Current) == false)
        {
            EscapedPath[InsertionIndex] = *Current;
            InsertionIndex += 1;
        }
        else
        {
            //
            // Insert the utf-8 sequence for the escaped character.
            //
            // N.B. The last byte of the sequence can hold only 6 bits of data
            //      due to utf-8 encoding, so the 2 most significant bits of
            //      the character are encoded in the second byte.
            //

            EscapedPath[InsertionIndex] = UtilEscapeCharBase[0];
            EscapedPath[InsertionIndex + 1] = UtilEscapeCharBase[1] | (*Current >> 6);
            EscapedPath[InsertionIndex + 2] = UtilEscapeCharBase[2] | (*Current & 0x3f);
            InsertionIndex += sizeof(UtilEscapeCharBase);
        }
    }
}

size_t EscapePathForNtLength(const char* Path)

/*++

Description:

    This routine determines the length needed to escape a Linux path for use
    in NT.

    N.B. The path is assumed to use Linux separators (forward slash), so those
         are not escaped.

Parameters:

    Path - Supplies the path to escape.

Return:

    The length in bytes. If this equals the length of the original string,
    there are no characters that need to e escaped.

--*/

{
    const char* Current;
    size_t Length;

    Length = 0;
    for (Current = Path; *Current != '\0'; Current += 1)
    {
        if (EscapeCharNeedsEscape(*Current) == false)
        {
            Length += 1;
        }
        else
        {
            Length += sizeof(UtilEscapeCharBase);
        }
    }

    return Length;
}

void UnescapePathInplace(char* Path)

/*++

Description:

    This routine unescapes the supplied string inplace.

Parameters:

    Path - Supplies the path to be unescaped.

Return:

    None.

--*/

{
    char* Current;
    char* Remaining;
    char Unescaped;

    for (Current = Path; *Current != '\0'; Current += 1)
    {
        //
        // If the current character is a utf-8 sequence that can be unescaped,
        // replace the character and shift down the remainder of the string.
        //

        if ((Current[0] == UtilEscapeCharBase[0]) && ((Current[1] & UtilEscapeCharBase[1]) != 0) &&
            ((Current[2] & UtilEscapeCharBase[2]) != 0))
        {
            Unescaped = (Current[1] << 6) | (Current[2] & 0x3f);
            if (EscapeCharNeedsEscape(Unescaped) != false)
            {
                *Current = Unescaped;
                Remaining = &Current[3];
                memmove(Current + 1, Remaining, strlen(Remaining) + 1);
            }
        }
    }
}
