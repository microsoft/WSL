// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once
#include <string>
#include <linux/rtnetlink.h>
#include <linux/rtnetlink.h>

inline std::string NetLinkFormatFlagsToString(int flags)
{
    if (flags == 0)
    {
        return "[zero]";
    }

    std::string returnString;
    if (flags & NLM_F_CREATE)
    {
        returnString.append("NLM_F_CREATE ");
    }
    if (flags & NLM_F_REPLACE)
    {
        returnString.append("NLM_F_REPLACE ");
    }
    if (flags & NLM_F_DUMP)
    {
        returnString.append("NLM_F_DUMP ");
    }
    if (flags & NLM_F_REQUEST)
    {
        returnString.append("NLM_F_REQUEST ");
    }
    if (flags & NLM_F_EXCL)
    {
        returnString.append("NLM_F_EXCL ");
    }
    return returnString;
}

inline auto* RouteOperationToString(int operation)
{
    if (operation == RTM_DELROUTE)
    {
        return "RTM_DELROUTE ";
    }
    if (operation == RTM_NEWROUTE)
    {
        return "RTM_NEWROUTE ";
    }
    return "[unknown route operation]";
}
