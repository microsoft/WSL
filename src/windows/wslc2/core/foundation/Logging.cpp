// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "Logging.h"
#include "DateTime.h"
#include "SharedThreadGlobals.h"
#include "StringUtilities.h"

namespace wsl::windows::wslc::logging
{
    std::wstring_view GetChannelName(Channel channel)
    {
        switch(channel)
        {
        case Channel::Fail:     return L"FAIL";
        case Channel::CLI:      return L"CLI";
        case Channel::Core:     return L"CORE";
        case Channel::Service:  return L"SVC";
        case Channel::Task:     return L"TASK";
        case Channel::Debug:     return L"DEBUG";
        default:                return L"NONE";
        }
    }

    Channel GetChannelFromName(std::wstring_view channel)
    {
        std::wstring lowerChannel = wsl::windows::wslc::util::ToLower(channel);

        if (lowerChannel == L"fail")
        {
            return Channel::Fail;
        }
        else if (lowerChannel == L"cli")
        {
            return Channel::CLI;
        }
        else if (lowerChannel == L"core")
        {
            return Channel::Core;
        }
        else if (lowerChannel == L"svc")
        {
            return Channel::Service;
        }
        else if (lowerChannel == L"task")
        {
            return Channel::Task;
        }
        else if (lowerChannel == L"debug")
        {
            return Channel::Debug;
        }
        else if (lowerChannel == L"default" || lowerChannel == L"defaults")
        {
            return Channel::Defaults;
        }
        else if (lowerChannel == L"all")
        {
            return Channel::All;
        }

        return Channel::None;
    }

    size_t GetMaxChannelNameLength() { return 4; }

    void DiagnosticLogger::AddLogger(std::unique_ptr<ILogger>&& logger)
    {
        m_loggers.emplace_back(std::move(logger));
    }

    bool DiagnosticLogger::ContainsLogger(const std::wstring& name)
    {
        for (auto i = m_loggers.begin(); i != m_loggers.end(); ++i)
        {
            if ((*i)->GetName() == name)
            {
                return true;
            }
        }

        return false;
    }

    std::unique_ptr<ILogger> DiagnosticLogger::RemoveLogger(const std::wstring& name)
    {
        std::unique_ptr<ILogger> result;

        for (auto i = m_loggers.begin(); i != m_loggers.end(); ++i)
        {
            if ((*i)->GetName() == name)
            {
                result = std::move(*i);
                m_loggers.erase(i);
                break;
            }
        }

        return result;
    }

    void DiagnosticLogger::RemoveAllLoggers()
    {
        m_loggers.clear();
    }

    void DiagnosticLogger::EnableChannel(Channel channel)
    {
        WI_SetAllFlags(m_enabledChannels, channel);
    }

    void DiagnosticLogger::SetEnabledChannels(Channel channel)
    {
        m_enabledChannels = channel;
    }

    void DiagnosticLogger::DisableChannel(Channel channel)
    {
        WI_ClearAllFlags(m_enabledChannels, channel);
    }

    void DiagnosticLogger::SetLevel(Level level)
    {
        m_enabledLevel = level;
    }

    Level DiagnosticLogger::GetLevel() const
    {
        return m_enabledLevel;
    }

    bool DiagnosticLogger::IsEnabled(Channel channel, Level level) const
    {
        return (!m_loggers.empty() &&
                WI_IsAnyFlagSet(m_enabledChannels, channel) &&
                (static_cast<int>(level) >= static_cast<int>(m_enabledLevel)));
    }

    void DiagnosticLogger::Write(Channel channel, Level level, std::wstring_view message)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, channel == Channel::All, "Cannot write to all channels");

        if (IsEnabled(channel, level))
        {
            for (auto& logger : m_loggers)
            {
                logger->Write(channel, level, message);
            }
        }
    }

    void DiagnosticLogger::WriteDirect(Channel channel, Level level, std::wstring_view message)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, channel == Channel::All, "Cannot write to all channels");

        if (IsEnabled(channel, level))
        {
            for (auto& logger : m_loggers)
            {
                logger->WriteDirect(channel, level, message);
            }
        }
    }

    void DiagnosticLogger::SetTag(Tag tag)
    {
        for (auto& logger : m_loggers)
        {
            logger->SetTag(tag);
        }
    }

    DiagnosticLogger& Log()
    {
        threadlocalstorage::ThreadGlobals* pThreadGlobals = threadlocalstorage::ThreadGlobals::GetForCurrentThread();
        if (pThreadGlobals)
        {
            return pThreadGlobals->GetDiagnosticLogger();
        }
        else
        {
            static DiagnosticLogger processGlobalLogger;
            return processGlobalLogger;
        }
    }

    std::wostream& SetHRFormat(std::wostream& out)
    {
        return out << std::hex << std::setw(8) << std::setfill(L'0');
    }
}

namespace std
{
    std::wostream& operator<<(std::wostream& out, const std::chrono::system_clock::time_point& time)
    {
        wsl::windows::wslc::util::OutputTimePoint(out, time);
        return out;
    }
}
