/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    BuildView.h

Abstract:

    This file contains the BuildView definition.

--*/
#pragma once

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace wsl::windows::common::docker_schema {
struct BuildKitSolveStatus;
}

namespace wsl::windows::wslc::services {

constexpr size_t c_maxLogLinesPerStep = 200;

struct SubStatus
{
    std::string id;
    int64_t current{};
    int64_t total{};
    bool completed{};
};

struct ViewStep
{
    std::string digest;
    std::string stepLabel;
    uint32_t stepNum{};
    uint32_t stepTotal{};
    bool started{};
    bool completed{};
    std::chrono::steady_clock::time_point startTime{};
    std::chrono::milliseconds elapsed{};
    bool cached{};
    std::string error;
    std::vector<std::string> inputs;
    std::string startedTs;
    std::vector<std::pair<std::string, SubStatus>> subStatuses;
    std::map<std::string, size_t> subIndex;
    std::vector<std::string> logOutput;
};

struct ViewTarget
{
    std::string name;
    std::vector<ViewStep> steps;
};

class BuildView
{
public:
    NON_COPYABLE(BuildView);
    BuildView();

    void ProcessMessage(const std::string& rawJson);
    std::vector<const ViewStep*> GetUnreportedSteps();

    const ViewStep* StepByDigest(const std::string& digest) const;
    bool HasErrors() const;

    std::chrono::steady_clock::time_point GetTotalStart() const
    {
        return m_totalStart;
    }

    const std::vector<ViewTarget>& GetTargets() const
    {
        return m_targets;
    }

private:
    std::unordered_map<std::string, std::pair<size_t, size_t>> m_digestIndex;
    std::unordered_map<std::string, size_t> m_targetNameToIdx;
    std::set<std::string> m_reportedSteps;
    std::chrono::steady_clock::time_point m_totalStart;
    std::vector<ViewTarget> m_targets;
};

// Utility functions
std::string ShortDigest(const std::string& id);
std::chrono::milliseconds ParseElapsed(const std::string& started, const std::string& completed);

struct ParsedTarget
{
    std::string targetName;
    std::string stepLabel;
    uint32_t stepNum{};
    uint32_t stepTotal{};
};

ParsedTarget ParseTarget(const std::string& name);

} // namespace wsl::windows::wslc::services
