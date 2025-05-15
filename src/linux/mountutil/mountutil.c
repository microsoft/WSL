// Copyright (C) Microsoft Corporation. All rights reserved.
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include "mountutil.h"

#define MOUNT_FIELD_SEP " "
#define MOUNT_OPTIONAL_FIELD_TERMINATOR "-"
#define MOUNT_DEVICE_SEP ':'
#define MOUNT_ESCAPE_CHAR '\\'
#define MOUNT_ESCAPE_LENGTH (3)

// Field indices of fields in the /proc/self/mountinfo file.
// N.B. There can be one or more optional fields, terminated by a single hyphen. This enumeration
//      counts all optional fields including the hyphen separator as a single field.
typedef enum _MOUNT_FIELD
{
    MountFieldId,
    MountFieldParentId,
    MountFieldDevice,
    MountFieldRoot,
    MountFieldMountPoint,
    MountFieldMountOptions,
    MountFieldOptionalFields,
    MountFieldFileSystemType,
    MountFieldSource,
    MountFieldSuperOptions,
    MountFieldMax = MountFieldSuperOptions
} MOUNT_FIELD,
    *PMOUNT_FIELD;

void MountFieldUnescape(char* field);

bool MountFieldUnescapeOctal(const char* string, char* unescaped);

char* MountNextField(char** line, MOUNT_FIELD field);

int MountParseDevice(char* field, dev_t* device);

// Initializes a MOUNT_ENUM structure.
int MountEnumCreate(PMOUNT_ENUM mountEnum)
{
    return MountEnumCreateEx(mountEnum, MOUNT_INFO_FILE);
}

// Initializes a MOUNT_ENUM structure from a custom mount info file.
int MountEnumCreateEx(PMOUNT_ENUM mountEnum, const char* mountInfoFile)
{
    int result;

    memset(mountEnum, 0, sizeof(*mountEnum));
    mountEnum->MountInfo = fopen(mountInfoFile, "r");
    if (mountEnum->MountInfo == NULL)
    {
        result = -1;
        goto MountEnumCreateEnd;
    }

    result = 0;

MountEnumCreateEnd:
    return result;
}

// Frees the resources held by a MOUNT_ENUM structure.
void MountEnumFree(PMOUNT_ENUM mountEnum)
{
    if (mountEnum->Line != NULL)
    {
        free(mountEnum->Line);
    }

    if (mountEnum->MountInfo != NULL)
    {
        fclose(mountEnum->MountInfo);
    }
}

// Reads the next line of the /proc/self/mountinfo file, and parses it.
int MountEnumNext(PMOUNT_ENUM mountEnum)
{
    ssize_t bytesRead;
    int result;

    do
    {
        // Get the next line.
        // N.B. Set errno to zero first so EOF can be distinguished.
        errno = 0;
        bytesRead = getline(&mountEnum->Line, &mountEnum->LineLength, mountEnum->MountInfo);
        if (bytesRead < 0)
        {
            result = -1;
            goto MountEnumNextEnd;
        }

        // Parse the line. Invalid lines are skipped.
        result = MountParseMountInfoLine(mountEnum->Line, &mountEnum->Current);
    } while (result < 0);

    result = 0;

MountEnumNextEnd:
    return result;
}

// Unescape a field in a mount entry that uses octal escape sequences.
void MountFieldUnescape(char* field)
{
    size_t index;
    size_t insertionIndex;
    char unescaped;

    // Scan until the end of the string.
    // N.B. The last field may contain a newline character, which is stripped
    //      by this function.
    for (index = 0, insertionIndex = 0; field[index] != '\0'; index += 1, insertionIndex += 1)
    {
        // If the character is a \, see if the following three characters can
        // be unescaped. Otherwise, move the current character back if a
        // previous escape sequence was encountered.
        if ((field[index] == MOUNT_ESCAPE_CHAR) && (MountFieldUnescapeOctal(&field[index + 1], &unescaped) != false))
        {
            field[insertionIndex] = unescaped;
            index += MOUNT_ESCAPE_LENGTH;
        }
        else if (insertionIndex < index)
        {
            field[insertionIndex] = field[index];
        }
    }

    // NULL terminate the field (if there were no escapes, this will just overwrite the existing
    // terminator).
    field[insertionIndex] = '\0';
}

// Unescape a single octal escape sequence.
bool MountFieldUnescapeOctal(const char* string, char* unescaped)
{
    char localUnescaped;
    char character;
    size_t index;

    localUnescaped = 0;
    for (index = 0; index < MOUNT_ESCAPE_LENGTH; index += 1)
    {
        character = string[index];
        if ((character < '0') || (character > '7'))
        {
            return false;
        }

        localUnescaped <<= 3;
        localUnescaped |= (character - '0');
    }

    *unescaped = localUnescaped;
    return true;
}

// Find the next field in the mountinfo line, and make sure it's NULL terminated.
// N.B. The start of the line is updated to the remaining text after the returned field.
char* MountNextField(char** line, MOUNT_FIELD field)
{
    if (field == 0)
    {
        return strtok_r(*line, MOUNT_FIELD_SEP, line);
    }
    else if (field < MountFieldMax)
    {
        return strtok_r(NULL, MOUNT_FIELD_SEP, line);
    }

    // The last field may contain separators, so don't look for the next separator in that case.
    // However, it may end with a newline.
    return strtok_r(NULL, "\n", line);
}

// Parse a device number of the form "major:minor" into a dev_t.
int MountParseDevice(char* field, dev_t* device)
{
    int major;
    int minor;
    int result;
    char* separator;

    separator = strchr(field, MOUNT_DEVICE_SEP);
    if (separator == NULL)
    {
        result = -1;
        goto MountParseDeviceEnd;
    }

    *separator = '\0';
    major = atoi(field);
    minor = atoi(separator + 1);
    *device = makedev(major, minor);
    result = 0;

MountParseDeviceEnd:
    return result;
}

// Parse a line from the /proc/self/mountinfo file.
int MountParseMountInfoLine(char* line, PMOUNT_ENTRY entry)
{
    char* current;
    MOUNT_FIELD field;
    int result;

    for (field = 0, current = MountNextField(&line, field); ((current != NULL) && (field <= MountFieldMax));
         field += 1, current = MountNextField(&line, field))
    {
        switch (field)
        {
        case MountFieldId:
            entry->Id = atoi(current);
            break;

        case MountFieldParentId:
            entry->ParentId = atoi(current);
            break;

        case MountFieldDevice:
            result = MountParseDevice(current, &entry->Device);
            if (result < 0)
            {
                goto ParseMountInfoLineEnd;
            }

        case MountFieldRoot:
            entry->Root = current;
            break;

        case MountFieldMountPoint:
            entry->MountPoint = current;
            break;

        case MountFieldMountOptions:
            entry->MountOptions = current;
            break;

        case MountFieldOptionalFields:
            // Find the end of the optional fields.
            while (current != NULL && strcmp(current, MOUNT_OPTIONAL_FIELD_TERMINATOR) != 0)
            {
                current = MountNextField(&line, field);
            }

            break;

        case MountFieldFileSystemType:
            entry->FileSystemType = current;
            break;

        case MountFieldSource:
            entry->Source = current;
            break;

        case MountFieldSuperOptions:
            entry->SuperOptions = current;
            break;
        }
    }

    // Check if all the fields were found. If not, this is a malformed line.
    if (field <= MountFieldMax)
    {
        result = -1;
        goto ParseMountInfoLineEnd;
    }

    // Decode any octal escape sequences in the fields that may be escaped.
    MountFieldUnescape(entry->Root);
    MountFieldUnescape(entry->Source);
    MountFieldUnescape(entry->MountPoint);
    result = 0;

ParseMountInfoLineEnd:
    return result;
}
