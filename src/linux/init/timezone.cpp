/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    timezone.c

Abstract:

    This file contains methods for configuring the timezone.

--*/

#include "common.h"
#include "util.h"
#include "WslDistributionConfig.h"

#define TIMEZONE_LOCALTIME_FILE ETC_FOLDER "localtime"
#define TIMEZONE_SETTING_FILE ETC_FOLDER "timezone"

void UpdateTimezone(std::string_view Timezone, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine updates the instance's timezone information by creating the
    /etc/localtime symlink and writing /etc/timezone.

Arguments:

    Timezone - Supplies the Linux timezone.

    Config - Supplies the distribution configuration.

Return Value:

    None.

--*/

try
{
    //
    // If automatic timezone translation is disabled, do nothing.
    //

    if (!Config.AutoUpdateTimezone)
    {
        return;
    }

    if (Timezone.empty())
    {
        LOG_WARNING("Windows to Linux timezone mapping was not possible.");
        return;
    }

    //
    // Construct the /etc/localtime symlink target and ensure it will exist.
    //

    std::string Target{"/usr/share/zoneinfo/"};
    Target += Timezone;
    if (access(Target.c_str(), F_OK) < 0)
    {
        LOG_WARNING("{} not found. Is the tzdata package installed?", Target.c_str());
        return;
    }

    //
    // Update the /etc/localtime symlink.
    //

    if ((unlink(TIMEZONE_LOCALTIME_FILE) < 0) && (errno != ENOENT))
    {
        LOG_ERROR("unlink failed {}", errno);
        return;
    }

    if (symlink(Target.c_str(), TIMEZONE_LOCALTIME_FILE) < 0)
    {
        LOG_ERROR("symlink failed {}", errno);
        return;
    }

    //
    // Write the contents of /etc/timezone to contain the IANA identifier.
    //

    wil::unique_fd TimezoneFile{TEMP_FAILURE_RETRY(open(TIMEZONE_SETTING_FILE, (O_CREAT | O_TRUNC | O_RDWR), 0644))};

    if (!TimezoneFile)
    {
        LOG_ERROR("open({}) failed {}", TIMEZONE_SETTING_FILE, errno);
        return;
    }

    std::string FileContents(Timezone);
    FileContents += '\n';
    if (UtilWriteStringView(TimezoneFile.get(), FileContents) < 0)
    {
        LOG_ERROR("write failed {}", errno);
        return;
    }

    return;
}
CATCH_LOG()

void UpdateTimezone(gsl::span<gsl::byte> Buffer, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine processes an update timezone message.

Arguments:

    Buffer - Supplies the message.

    Config - Supplies the distribution configuration.


Return Value:

    None.

--*/

{
    auto* TimezoneInfo = gslhelpers::try_get_struct<const LX_INIT_TIMEZONE_INFORMATION>(Buffer);
    if (!TimezoneInfo)
    {
        LOG_ERROR("Unexpected message size {}", Buffer.size());
        return;
    }

    UpdateTimezone(wsl::shared::string::FromSpan(Buffer, TimezoneInfo->TimezoneOffset), Config);
}
