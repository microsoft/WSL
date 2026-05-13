/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    BuildView.cpp

Abstract:

    This file contains the BuildView implementation

--*/
#include "precomp.h"
#include "BuildView.h"
#include <docker_schema.h>
#include <wslutil.h>

using namespace wsl::windows::common;

namespace wsl::windows::wslc::services {

// ── Utility functions ────────────────────────────────────────────────

std::string ShortDigest(const std::string& id)
{
    constexpr auto prefix = "sha256:";
    constexpr size_t prefixLen = 7;
    if (id.starts_with(prefix))
    {
        auto rest = id.substr(prefixLen, 8);
        return std::string(prefix) + rest;
    }
    return id;
}

std::chrono::milliseconds ParseElapsed(const std::string& started, const std::string& completed)
{
    // Parse nanosecond-precision timestamp for difference calculation.
    auto parseNanos = [](const std::string& s) -> std::optional<uint64_t> {
        auto tPos = s.find('T');
        if (tPos == std::string::npos)
        {
            return std::nullopt;
        }

        auto datePart = s.substr(0, tPos);
        auto timeRest = s.substr(tPos + 1);

        // Strip timezone suffix (Z, +HH:MM, -HH:MM)
        std::string timePart;
        if (timeRest.ends_with('Z'))
        {
            timePart = timeRest.substr(0, timeRest.size() - 1);
        }
        else if (timeRest.size() >= 6)
        {
            auto tail = timeRest.substr(timeRest.size() - 6);
            if ((tail[0] == '+' || tail[0] == '-') && tail[3] == ':')
            {
                timePart = timeRest.substr(0, timeRest.size() - 6);
            }
            else
            {
                timePart = timeRest;
            }
        }
        else
        {
            timePart = timeRest;
        }

        // Parse date: YYYY-MM-DD
        int year = 0, month = 0, day = 0;
        if (sscanf_s(datePart.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
        {
            return std::nullopt;
        }

        // Parse time: HH:MM:SS[.fractional]
        int hour = 0, min = 0, sec = 0;
        uint64_t fracNanos = 0;
        auto dotPos = timePart.find('.');
        if (dotPos != std::string::npos)
        {
            if (sscanf_s(timePart.c_str(), "%d:%d:%d", &hour, &min, &sec) != 3)
            {
                return std::nullopt;
            }
            auto fracStr = timePart.substr(dotPos + 1);
            fracNanos = std::stoull(fracStr);
            for (size_t i = fracStr.size(); i < 9; i++)
            {
                fracNanos *= 10;
            }
        }
        else
        {
            if (sscanf_s(timePart.c_str(), "%d:%d:%d", &hour, &min, &sec) != 3)
            {
                return std::nullopt;
            }
        }

        // Use std::tm + _mkgmtime for correct calendar-to-seconds conversion.
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        auto epochSecs = static_cast<uint64_t>(_mkgmtime(&tm));
        return epochSecs * 1'000'000'000ULL + fracNanos;
    };

    auto s = parseNanos(started);
    auto c = parseNanos(completed);
    if (s && c && *c >= *s)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(*c - *s));
    }
    return std::chrono::milliseconds::zero();
}

ParsedTarget ParseTarget(const std::string& name)
{
    if (!name.empty() && name[0] == '[')
    {
        auto close = name.find(']');
        if (close != std::string::npos)
        {
            auto inside = name.substr(1, close - 1);

            // Tokenize by whitespace
            std::vector<std::string> tokens;
            std::istringstream iss(inside);
            std::string token;
            while (iss >> token)
            {
                tokens.push_back(token);
            }

            uint32_t stepNum = 0, stepTotal = 0;
            for (const auto& t : tokens)
            {
                auto slash = t.find('/');
                if (slash != std::string::npos)
                {
                    try
                    {
                        stepNum = static_cast<uint32_t>(std::stoul(t.substr(0, slash)));
                        stepTotal = static_cast<uint32_t>(std::stoul(t.substr(slash + 1)));
                    }
                    catch (...)
                    {
                    }
                }
            }

            auto firstToken = tokens.empty() ? inside : tokens[0];

            if (!firstToken.empty() && std::isdigit(static_cast<unsigned char>(firstToken[0])))
            {
                return {"default", name, stepNum, stepTotal};
            }

            return {firstToken, name, stepNum, stepTotal};
        }
    }

    if (name.starts_with("exporting") || name.starts_with("writing") || name.starts_with("naming"))
    {
        return {"export", name, 0, 0};
    }
    return {"other", name, 0, 0};
}

// ── BuildView ────────────────────────────────────────────────────────

BuildView::BuildView() : m_totalStart(std::chrono::steady_clock::now())
{
}

void BuildView::ProcessMessage(const wsl::windows::common::docker_schema::BuildKitSolveStatus& msg)
{
    std::vector<size_t> targetsToSort;

    // Phase 1: Process vertexes
    for (const auto& vertex : msg.vertexes)
    {
        size_t targetIdx, stepIdx;

        auto it = m_digestIndex.find(vertex.digest);
        if (it != m_digestIndex.end())
        {
            targetIdx = it->second.first;
            stepIdx = it->second.second;
        }
        else
        {
            auto parsed = ParseTarget(vertex.name);

            auto targetIt = m_targetNameToIdx.find(parsed.targetName);
            if (targetIt != m_targetNameToIdx.end())
            {
                targetIdx = targetIt->second;
            }
            else
            {
                targetIdx = m_targets.size();
                m_targets.push_back(ViewTarget{parsed.targetName, {}});
                m_targetNameToIdx[parsed.targetName] = targetIdx;
            }

            ViewStep newStep{};
            newStep.digest = vertex.digest;
            newStep.stepLabel = parsed.stepLabel;
            newStep.stepNum = parsed.stepNum;
            newStep.stepTotal = parsed.stepTotal;
            newStep.inputs = vertex.inputs;

            stepIdx = m_targets[targetIdx].steps.size();
            m_targets[targetIdx].steps.push_back(std::move(newStep));
            m_digestIndex[vertex.digest] = {targetIdx, stepIdx};

            if (std::find(targetsToSort.begin(), targetsToSort.end(), targetIdx) == targetsToSort.end())
            {
                targetsToSort.push_back(targetIdx);
            }
        }

        auto& step = m_targets[targetIdx].steps[stepIdx];

        // Update the step's lifecycle state.
        // BuildKit FROM steps go through multiple start→complete cycles
        // (manifest resolution, then layer pulling). We must track the
        // LATEST start timestamp so the elapsed timer reflects the full
        // pull duration, not just the first sub-second manifest lookup.
        if (!vertex.started.empty())
        {
            if (!step.started)
            {
                step.started = true;
            }
            // A new started timestamp (without completed) means the step
            // is restarting for a new phase — mark it active again.
            if (vertex.completed.empty() && step.completed)
            {
                step.completed = false;
                step.startTime = std::chrono::steady_clock::now();
            }
            // Always update to the latest started timestamp.
            step.startedTs = vertex.started;
            if (step.startTime == std::chrono::steady_clock::time_point{})
            {
                step.startTime = std::chrono::steady_clock::now();
            }
        }

        if (vertex.cached)
        {
            step.cached = true;
        }

        if (!vertex.completed.empty())
        {
            step.completed = true;
            if (!vertex.error.empty())
            {
                auto error = vertex.error;
                std::replace(error.begin(), error.end(), '\n', ' ');
                step.error = std::move(error);
            }

            if (!step.startedTs.empty())
            {
                step.elapsed = ParseElapsed(step.startedTs, vertex.completed);
            }
            else if (step.startTime != std::chrono::steady_clock::time_point{})
            {
                step.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - step.startTime);
            }
        }
    }

    // Phase 2: Re-sort steps within targets that received new steps
    for (auto targetIdx : targetsToSort)
    {
        std::sort(m_targets[targetIdx].steps.begin(), m_targets[targetIdx].steps.end(), [](const ViewStep& a, const ViewStep& b) {
            return a.stepNum < b.stepNum;
        });
        for (size_t si = 0; si < m_targets[targetIdx].steps.size(); si++)
        {
            m_digestIndex[m_targets[targetIdx].steps[si].digest] = {targetIdx, si};
        }
    }

    // Phase 3: Process statuses (layer download/extract progress)
    for (const auto& status : msg.statuses)
    {
        auto it = m_digestIndex.find(status.vertex);
        if (it == m_digestIndex.end())
        {
            continue;
        }

        auto& step = m_targets[it->second.first].steps[it->second.second];

        auto subIt = step.subIndex.find(status.id);
        size_t subIdx;
        if (subIt != step.subIndex.end())
        {
            subIdx = subIt->second;
        }
        else
        {
            subIdx = step.subStatuses.size();
            step.subStatuses.push_back({status.id, SubStatus{status.id, 0, 0, false}});
            step.subIndex[status.id] = subIdx;
        }

        auto& sub = step.subStatuses[subIdx].second;
        sub.current = status.current;
        if (status.total > 0)
        {
            sub.total = status.total;
        }
        if (!status.completed.empty())
        {
            sub.completed = true;
        }
    }

    // Phase 4: Process logs (base64-encoded build output)
    for (const auto& logEntry : msg.logs)
    {
        auto it = m_digestIndex.find(logEntry.vertex);
        if (it == m_digestIndex.end())
        {
            continue;
        }

        auto& step = m_targets[it->second.first].steps[it->second.second];
        std::string text = wslutil::Base64Decode(logEntry.data);

        // Trim trailing whitespace.
        auto endPos = text.find_last_not_of(" \n\r\t");
        text = (endPos != std::string::npos) ? text.substr(0, endPos + 1) : "";

        // Trim leading whitespace.
        auto startPos = text.find_first_not_of(" \n\r\t");
        text = (startPos != std::string::npos) ? text.substr(startPos) : "";

        if (text.empty())
        {
            continue;
        }

        // Split on newlines
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            // Trim each sub-line
            auto lineEnd = line.find_last_not_of(" \r\t");
            line = (lineEnd != std::string::npos) ? line.substr(0, lineEnd + 1) : "";
            auto lineStart = line.find_first_not_of(" \r\t");
            line = (lineStart != std::string::npos) ? line.substr(lineStart) : "";
            if (!line.empty())
            {
                step.logOutput.push_back(std::move(line));
            }
        }
    }
}

const ViewStep* BuildView::StepByDigest(const std::string& digest) const
{
    auto it = m_digestIndex.find(digest);
    if (it == m_digestIndex.end())
    {
        return nullptr;
    }
    return &m_targets[it->second.first].steps[it->second.second];
}

bool BuildView::HasErrors() const
{
    for (const auto& target : m_targets)
    {
        for (const auto& step : target.steps)
        {
            if (!step.error.empty())
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace wsl::windows::wslc::services
