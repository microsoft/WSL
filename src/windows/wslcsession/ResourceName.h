// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

namespace wsl::windows::service::wslc {

inline void ValidateName(LPCSTR Name, size_t maxLength)
{
    const auto& locale = std::locale::classic();
    size_t i = 0;

    for (; Name[i] != '\0'; i++)
    {
        if (!std::isalnum(Name[i], locale) && Name[i] != '_' && Name[i] != '-' && Name[i] != '.')
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageWslcInvalidName(Name));
        }
    }

    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, wsl::shared::Localization::MessageWslcInvalidName(Name), i == 0 || i > maxLength);
}

} // namespace wsl::windows::service::wslc
