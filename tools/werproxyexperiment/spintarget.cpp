// Copyright (C) Microsoft Corporation. All rights reserved.
//
// Process A of the WER proxy reporting experiment.
//
// Spins on QueryPerformanceCounter and records every interruption in its own execution, so that a
// report submitted against this process by proxyreport.exe (Process B) can be checked for whether
// WER suspended the process while collecting its dump.
//
// QPC is a system wide timebase, so the raw counter values printed here line up with the ones
// printed by Process B and a gap can be attributed to a specific WER call.
//
// Gaps are recorded per thread. A gap on a single thread is ordinary scheduling; a gap covering the
// same interval on every thread means the whole process was stopped.

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Gap
{
    long long StartQpc{};
    long long EndQpc{};
    double Milliseconds{};
};

struct ThreadResult
{
    unsigned long long Iterations{};
    std::vector<Gap> Gaps;
};

// Enough to characterize a run without letting a small threshold grow the vector without bound.
constexpr size_t c_maxRecordedGaps = 4096;

std::atomic<bool> g_stop{false};
long long g_frequency = 0;

long long Now()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double ToMilliseconds(long long Ticks)
{
    return (static_cast<double>(Ticks) * 1000.0) / static_cast<double>(g_frequency);
}

BOOL WINAPI ControlHandler(DWORD)
{
    g_stop.store(true, std::memory_order_relaxed);
    return TRUE;
}

void Spin(ThreadResult* Result, double ThresholdMs, bool HighPriority)
{
    if (HighPriority)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    Result->Gaps.reserve(c_maxRecordedGaps);

    auto last = Now();
    while (!g_stop.load(std::memory_order_relaxed))
    {
        const auto now = Now();
        const auto elapsed = ToMilliseconds(now - last);
        if (elapsed >= ThresholdMs && Result->Gaps.size() < c_maxRecordedGaps)
        {
            Result->Gaps.push_back({last, now, elapsed});
        }

        last = now;
        Result->Iterations += 1;
    }
}

// Whether the interval also appears on another thread, which is what distinguishes a suspended
// process from a descheduled thread.
bool OverlapsThread(const Gap& Interval, const ThreadResult& Other)
{
    return std::any_of(Other.Gaps.begin(), Other.Gaps.end(), [&](const Gap& e) {
        return e.StartQpc < Interval.EndQpc && Interval.StartQpc < e.EndQpc;
    });
}

void PrintUsage()
{
    wprintf(
        L"Usage: winget.exe [options]\n"
        L"\n"
        L"  --duration <seconds>   How long to spin before printing the summary (default 60).\n"
        L"  --threads <count>      Spinning threads. More than one is needed to tell a suspended\n"
        L"                         process apart from a descheduled thread (default 2).\n"
        L"  --threshold <ms>       Smallest interruption to record (default 5).\n"
        L"  --high-priority        Raise the spinning threads to time critical, lowering the\n"
        L"                         scheduling noise a real suspension has to stand out from.\n");
}

} // namespace

int wmain(int Argc, wchar_t** Argv)
{
    int durationSeconds = 60;
    int threadCount = 2;
    double thresholdMs = 5.0;
    bool highPriority = false;

    for (int i = 1; i < Argc; i += 1)
    {
        const std::wstring argument{Argv[i]};
        auto value = [&]() -> const wchar_t* { return (i + 1 < Argc) ? Argv[++i] : L""; };

        if (argument == L"--duration")
        {
            durationSeconds = _wtoi(value());
        }
        else if (argument == L"--threads")
        {
            threadCount = std::max(1, _wtoi(value()));
        }
        else if (argument == L"--threshold")
        {
            thresholdMs = _wtof(value());
        }
        else if (argument == L"--high-priority")
        {
            highPriority = true;
        }
        else
        {
            PrintUsage();
            return (argument == L"--help") ? 0 : 1;
        }
    }

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_frequency = frequency.QuadPart;

    wchar_t imagePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, imagePath, ARRAYSIZE(imagePath));

    SetConsoleCtrlHandler(ControlHandler, TRUE);

    const auto start = Now();

    wprintf(L"Process A\n");
    wprintf(L"  pid           : %lu\n", GetCurrentProcessId());
    wprintf(L"  image         : %ls\n", imagePath);
    wprintf(L"  qpc frequency : %lld\n", g_frequency);
    wprintf(L"  qpc at start  : %lld\n", start);
    wprintf(L"  threads       : %d\n", threadCount);
    wprintf(L"  gap threshold : %.1f ms\n\n", thresholdMs);
    wprintf(L"Report against pid %lu from Process B now.\n\n", GetCurrentProcessId());
    fflush(stdout);

    std::vector<ThreadResult> results(static_cast<size_t>(threadCount));
    std::vector<std::thread> threads;
    for (auto& result : results)
    {
        threads.emplace_back([&result, thresholdMs, highPriority]() { Spin(&result, thresholdMs, highPriority); });
    }

    for (int elapsed = 0; elapsed < durationSeconds && !g_stop.load(std::memory_order_relaxed); elapsed += 1)
    {
        Sleep(1000);

        double worst = 0.0;
        for (const auto& result : results)
        {
            for (const auto& gap : result.Gaps)
            {
                worst = std::max(worst, gap.Milliseconds);
            }
        }

        wprintf(L"  t+%02ds  worst gap so far: %.2f ms\n", elapsed + 1, worst);
        fflush(stdout);
    }

    g_stop.store(true, std::memory_order_relaxed);
    for (auto& thread : threads)
    {
        thread.join();
    }

    wprintf(L"\nSummary\n");

    std::vector<std::pair<size_t, Gap>> allGaps;
    for (size_t i = 0; i < results.size(); i += 1)
    {
        wprintf(L"  thread %zu: %llu iterations, %zu gaps\n", i, results[i].Iterations, results[i].Gaps.size());
        for (const auto& gap : results[i].Gaps)
        {
            allGaps.emplace_back(i, gap);
        }
    }

    if (allGaps.empty())
    {
        wprintf(L"\n  No interruption above %.1f ms. The process was never stopped.\n", thresholdMs);
        return 0;
    }

    std::sort(allGaps.begin(), allGaps.end(), [](const auto& left, const auto& right) {
        return left.second.Milliseconds > right.second.Milliseconds;
    });

    wprintf(L"\n  Largest interruptions:\n");
    for (size_t i = 0; i < std::min(size_t{10}, allGaps.size()); i += 1)
    {
        const auto& entry = allGaps[i];

        size_t overlapping = 1;
        for (size_t other = 0; other < results.size(); other += 1)
        {
            if (other != entry.first && OverlapsThread(entry.second, results[other]))
            {
                overlapping += 1;
            }
        }

        wprintf(
            L"    %8.2f ms  at t+%7.2f s  qpc %lld..%lld  thread %zu  (seen on %zu/%zu threads)%ls\n",
            entry.second.Milliseconds,
            ToMilliseconds(entry.second.StartQpc - start) / 1000.0,
            entry.second.StartQpc,
            entry.second.EndQpc,
            entry.first,
            overlapping,
            results.size(),
            (overlapping == results.size() && results.size() > 1) ? L"  <-- whole process stopped" : L"");
    }

    return 0;
}
