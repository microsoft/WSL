/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Plugin.cpp

Abstract:

    This file contains a test plugin.

--*/

#include "precomp.h"
#include <atomic>
#include <thread>
#include "WslPluginApi.h"
#include "wslc_schema.h"

#include <atomic>
#include <future>
#include <mutex>

#include "PluginTests.h"

using namespace wsl::windows::common::registry;
using namespace wsl::windows::common::relay;
using namespace wsl::shared::string;
using namespace std::chrono_literals;

std::ofstream g_logfile;
std::optional<GUID> g_distroGuid;

const WSLPluginAPIV1* g_api = nullptr;
PluginTestType g_testType = PluginTestType::Invalid;

// Process deliberately left running across OnWslcVmStopping by the WslcVmStopCommitted test, to
// prove the announced teardown happens anyway. Never released: it dies with the VM.
//
// The exit event is fetched on the callback's own thread and cached here as a plain Win32 handle:
// the process itself is a COM proxy marshalled to that thread, so the stop-window thread below
// cannot call methods on it (RPC_E_WRONG_THREAD), but it can wait on the handle.
std::atomic<WSLCProcessHandle> g_leakedProcess = nullptr;
std::atomic<HANDLE> g_leakedProcessExitEvent = nullptr;

// Set by the WslcVmStopCommitted test: a call issued from a thread the plugin owns while
// OnWslcVmStopping is running. Like the callback itself it is served by the VM that is stopping, and
// must not block on the teardown. It deliberately logs nothing of its own -- its results are written
// when it is joined -- so g_logfile keeps a single writer and the expected output stays ordered.
std::thread g_stopWindowCaller;
HRESULT g_stopWindowCallerResult = E_PENDING;
std::atomic<bool> g_leakedProcessDied = false;

std::optional<uint32_t> g_previousInitPid;

// Serializes writes to g_logfile from multiple threads in modes that spawn
// worker threads (ConcurrentApiCalls, AsyncApiCall, CallbackDuringTermination).
// Hook-thread writes that don't overlap with worker writes don't need to take
// this — but it's harmless to do so.
std::mutex g_logMutex;

void LogLine(const std::string& line)
{
    std::lock_guard guard{g_logMutex};
    g_logfile << line << std::endl;
}

// State for AsyncApiCall: worker thread launched in OnDistroStarted, joined
// in OnDistroStopping. The promise carries the result so the hook can log it
// after the join. The future is retrieved exactly once (in OnDistroStarted)
// and consumed in OnDistroStopping — std::promise::get_future() can only be
// called once per promise instance.
std::optional<std::thread> g_asyncWorker;
std::optional<std::promise<HRESULT>> g_asyncWorkerResult;
std::future<HRESULT> g_asyncWorkerFuture;
std::string g_asyncWorkerOutput;

// State for CallbackDuringTermination. Workers loop ExecuteBinaryInDistribution
// while the distro is alive. OnVmStopping (which fires before _VmTerminate's
// exclusive-lock drain) sets g_drainWindDown; workers then keep racing the drain
// for a bounded number of iterations and exit. Exiting on a fixed count (rather
// than on a post-reset failure) keeps shutdown deterministic and prevents a
// worker from "reviving" against a VM that a later StartWsl creates with the
// same session/distro IDs.
//
// g_drainWorkersStarted prevents the post-shutdown StartWsl (which the test uses
// to verify the service survived) from spawning a fresh batch; that second
// OnDistroStarted instead joins the finished workers. Threads are kept joinable
// in g_drainWorkers so the test needs no fixed sleep.
constexpr int c_drainWindDownIterations = 100;
std::atomic<int> g_drainSuccess{0};
std::atomic<int> g_drainFailures{0};
std::atomic<bool> g_drainWindDown{false};
std::atomic<bool> g_drainWorkersStarted{false};
std::vector<std::thread> g_drainWorkers;

// State for ParallelWslcWithCallbacks. Two sessions created in parallel, each
// spawns a worker that waits at a barrier before making a callback. The barrier
// ensures both callbacks execute concurrently (maxConcurrent=2).
std::atomic<int> g_parallelStarted{0};
std::atomic<int> g_parallelSuccess{0};
std::atomic<int> g_parallelMaxConcurrent{0};
std::atomic<bool> g_parallelBarrierReached{false};
std::vector<std::thread> g_parallelWorkers;
std::mutex g_parallelWorkersLock;

// State for WslcAsyncCallbackPostReturn. The hook spawns a worker, returns,
// then the worker sleeps (to ensure the pump is unregistered) and calls back.
// Joined in OnWslcSessionStopping, so output appears before "Session stopping".
std::optional<std::thread> g_wslcAsyncWorker;
WSLCSessionId g_wslcAsyncSessionId = 0;

std::vector<char> ReadFromSocket(SOCKET socket)
{
    // Simplified error handling for the sake of the demo.
    int result = 0;
    int offset = 0;

    std::vector<char> content(1024);
    while ((result = recv(socket, content.data() + offset, 1024, 0)) > 0)
    {
        offset += result;
        content.resize(offset + 1024);
    }

    content.resize(offset);
    return content;
}

HRESULT OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings)
{
    g_logfile << "VM created (settings->CustomConfigurationFlags=" << Settings->CustomConfigurationFlags << ")" << std::endl;

    if (g_testType == PluginTestType::FailToStartVm)
    {
        g_logfile << "OnVmStarted: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::FailToStartVmWithPluginErrorMessage)
    {
        g_logfile << "OnVmStarted: E_UNEXPECTED" << std::endl;
        g_api->PluginError(L"Plugin error message");
        return E_UNEXPECTED;
    }
    else if (WI_IsFlagSet(Settings->CustomConfigurationFlags, WSLUserConfigurationCustomKernel))
    {
        g_logfile << "OnVmStarted: E_ACCESSDENIED" << std::endl;
        return E_ACCESSDENIED;
    }
    else if (g_testType == PluginTestType::Success)
    {
        // Get the current module's directory
        std::filesystem::path modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle()).get();
        auto mountSource = modulePath.parent_path().wstring();

        // Mount the folder with the linux binary in the vm
        RETURN_IF_FAILED(
            g_api->MountFolder(Session->SessionId, mountSource.c_str(), L"/test-plugin/deep/folder", true, L"test-plugin-mount"));

        g_logfile << "Folder mounted (" << wsl::shared::string::WideToMultiByte(mountSource) << " -> /test-plugin)" << std::endl;

        // Create a file with dummy content
        std::ofstream file(mountSource + L"\\test-file.txt");
        if (!file || !(file << "OK"))
        {
            g_logfile << "Failed to open test-file.txt in: " << wsl::shared::string::WideToMultiByte(mountSource) << std::endl;
            return E_ABORT;
        }

        file.close();

        // Launch the process
        std::vector<const char*> arguments = {"/bin/cat", "/test-plugin/deep/folder/test-file.txt", nullptr};
        wil::unique_socket socket;
        RETURN_IF_FAILED(g_api->ExecuteBinary(Session->SessionId, arguments[0], arguments.data(), &socket));
        g_logfile << "Process created" << std::endl;

        // Read the socket output
        auto output = ReadFromSocket(socket.get());
        if (output != std::vector<char>{'O', 'K'})
        {
            g_logfile << "Got unexpected output from bash" << std::endl;
            return E_ABORT;
        }
    }
    else if (g_testType == PluginTestType::MountFolderAccess)
    {
        const auto key = OpenTestRegistryKey(KEY_READ);
        const auto mountSource = ReadString(key.get(), nullptr, c_mountFolder);

        RETURN_IF_FAILED(
            g_api->MountFolder(Session->SessionId, mountSource.c_str(), L"/test-plugin-access", false, L"test-plugin-access"));

        std::vector<const char*> arguments = {
            "/bin/sh",
            "-c",
            "{ echo allowed > /test-plugin-access/allowed/plugin-allowed.txt; echo denied > /test-plugin-access/plugin-test.txt; "
            "} 2>&1",
            nullptr};
        wil::unique_socket socket;
        RETURN_IF_FAILED(g_api->ExecuteBinary(Session->SessionId, arguments[0], arguments.data(), &socket));

        const auto output = ReadFromSocket(socket.get());
        g_logfile.write(output.data(), output.size());
    }
    else if (g_testType == PluginTestType::ApiErrors)
    {
        auto result = g_api->MountFolder(Session->SessionId, L"C:\\DoesNotExit", L"/dummy", true, L"test-plugin-mount");
        if (result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        {
            g_logfile << "Unexpected error for MountFolder(): " << result << std::endl;
            return E_ABORT;
        }

        wil::unique_socket socket;
        std::vector<const char*> arguments = {"/bin/does-no-exist", nullptr};
        result = g_api->ExecuteBinary(Session->SessionId, arguments[0], arguments.data(), &socket);
        if (result != E_FAIL)
        {
            g_logfile << "Unexpected error for ExecuteBinary(): " << result << std::endl;
            return E_ABORT;
        }

        result = g_api->ExecuteBinary(0xcafe, arguments[0], arguments.data(), &socket);
        if (result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            g_logfile << "Unexpected error for ExecuteBinary(): " << result << std::endl;
            return E_ABORT;
        }

        // Call PluginError asynchronously to verify that we handle this properly.

        std::thread thread{[Session]() {
            const auto result = g_api->PluginError(L"Dummy");

            if (result != E_ILLEGAL_METHOD_CALL)
            {
                g_logfile << "Unexpected error for async PluginError(): " << result << std::endl;
            }
        }};

        thread.join();

        g_logfile << "API error tests passed" << std::endl;
    }
    else if (g_testType == PluginTestType::ErrorMessageStartVm)
    {
        auto result = g_api->PluginError(L"StartVm plugin error message");
        if (FAILED(result))
        {
            g_logfile << "Unexpected error from PluginError(): " << result << std::endl;
        }
        g_logfile << "OnVmStarted: E_FAIL" << std::endl;
        return E_FAIL;
    }
    else if (g_testType == PluginTestType::GetUsername)
    {
        try
        {
            auto info = wil::get_token_information<TOKEN_USER>(Session->UserToken);

            DWORD size{};
            DWORD domainSize{};
            SID_NAME_USE use{};
            LookupAccountSid(nullptr, info->User.Sid, nullptr, &size, nullptr, &domainSize, &use);

            THROW_HR_IF(E_UNEXPECTED, size < 1);
            std::wstring user(size - 1, '\0');
            std::wstring domain(domainSize - 1, '\0');

            THROW_IF_WIN32_BOOL_FALSE(LookupAccountSid(nullptr, info->User.Sid, user.data(), &size, domain.data(), &domainSize, &use));

            g_logfile << "Username: " << wsl::shared::string::WideToMultiByte(domain) << "\\"
                      << wsl::shared::string::WideToMultiByte(user) << std::endl;
        }
        catch (...)
        {
            g_logfile << "OnVmStarted: get_token_information failed: " << wil::ResultFromCaughtException() << std::endl;
            return E_FAIL;
        }

        return S_OK;
    }
    else if (g_testType == PluginTestType::HostCrash)
    {
        // Validate plugin host crash handling. Forcefully exit the host process
        // so the COM RPC returns one of the HRESULTs in IsHostCrash
        // (RPC_E_DISCONNECTED / RPC_E_SERVER_DIED / ...). The service treats a
        // crash during a veto hook as a fatal plugin error and aborts the
        // operation.
        LogLine("Crashing host");
        g_logfile.flush();
        TerminateProcess(GetCurrentProcess(), 1);
        // Unreachable.
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::ConcurrentApiCalls)
    {
        // Validate service-side callbacks issued by multiple plugin threads
        // during a hook. N threads call MountFolder + ExecuteBinary via a
        // start-gate so the RPCs are all in flight at once.
        //
        // maxConcurrent records how many workers are simultaneously at the
        // plugin-side callback boundary (via a second rendezvous). Reaching N
        // proves the plugin issues N callbacks concurrently. It does NOT prove
        // the service executes them in parallel: with the PluginCallPump the
        // service marshals these onto the single notifying thread and runs them
        // serially. The test asserts only that all N succeed, which is the
        // strongest honest black-box assertion.
        constexpr int N = 4;

        std::filesystem::path modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle()).get();
        const auto mountSource = modulePath.parent_path().wstring();

        std::mutex gateMutex;
        std::condition_variable gateCv;
        int arrived = 0;
        bool released = false;
        int inFlight = 0;
        int maxConcurrent = 0;

        std::atomic<int> successes{0};
        std::atomic<int> failures{0};

        const auto worker = [&](int index) {
            // Gate 1: wait for all workers to be spawned so they overlap.
            {
                std::unique_lock lock{gateMutex};
                ++arrived;
                if (arrived == N)
                {
                    released = true;
                    gateCv.notify_all();
                }
                else
                {
                    gateCv.wait(lock, [&]() { return released; });
                }
            }

            // Gate 2: rendezvous right before the callbacks so all N are at the
            // boundary at once, making maxConcurrent == N deterministic.
            {
                std::unique_lock lock{gateMutex};
                ++inFlight;
                maxConcurrent = std::max(maxConcurrent, inFlight);
                if (inFlight == N)
                {
                    gateCv.notify_all();
                }
                else
                {
                    gateCv.wait(lock, [&]() { return inFlight == N; });
                }
            }

            const auto linuxPath = L"/test-plugin/concurrent-" + std::to_wstring(index);
            const auto mountName = L"test-plugin-concurrent-" + std::to_wstring(index);
            HRESULT hr = g_api->MountFolder(Session->SessionId, mountSource.c_str(), linuxPath.c_str(), true, mountName.c_str());
            if (FAILED(hr))
            {
                ++failures;
                return;
            }

            wil::unique_socket socket;
            std::vector<const char*> args = {"/bin/true", nullptr};
            hr = g_api->ExecuteBinary(Session->SessionId, args[0], args.data(), &socket);
            if (FAILED(hr))
            {
                ++failures;
                return;
            }

            ++successes;
        };

        std::vector<std::thread> threads;
        threads.reserve(N);
        for (int i = 0; i < N; ++i)
        {
            threads.emplace_back(worker, i);
        }
        for (auto& t : threads)
        {
            t.join();
        }

        LogLine(
            "Concurrent callbacks complete: success=" + std::to_string(successes.load()) +
            " failures=" + std::to_string(failures.load()) + " maxConcurrent=" + std::to_string(maxConcurrent));

        if (failures.load() != 0)
        {
            return E_FAIL;
        }
    }

    return S_OK;
}

HRESULT OnVmStopping(const WSLSessionInformation* Session)
{
    if (g_testType == PluginTestType::CallbackDuringTermination)
    {
        // Signal drain workers to begin a bounded wind-down. Fires before
        // _VmTerminate resets m_utilityVm, so workers keep racing teardown for
        // a fixed number of iterations before exiting.
        g_drainWindDown = true;
    }

    g_logfile << "VM Stopping" << std::endl;

    if (g_testType == PluginTestType::FailToStopVm)
    {
        g_logfile << "OnVmStopping: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    return S_OK;
}

HRESULT OnDistroStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    g_logfile << "Distribution started, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
              << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
              << ", PidNs=" << Distribution->PidNamespace << ", InitPid=" << Distribution->InitPid
              << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
              << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;

    if (g_testType == PluginTestType::FailToStartDistro)
    {
        g_logfile << "OnDistroStarted: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::SameDistroId)
    {
        if (g_distroGuid.has_value())
        {
            if (IsEqualGUID(g_distroGuid.value(), Distribution->Id))
            {
                g_logfile << "OnDistroStarted: received same GUID" << std::endl;
            }
            else
            {
                g_logfile << "OnDistroStarted: received different GUID" << std::endl;
            }
        }
        else
        {
            g_distroGuid = Distribution->Id;
        }
    }
    else if (g_testType == PluginTestType::ErrorMessageStartDistro)
    {
        g_logfile << "OnDistroStarted: E_FAIL" << std::endl;
        g_api->PluginError(L"StartDistro plugin error message");
        return E_FAIL;
    }
    else if (g_testType == PluginTestType::InitPidIsDifferent)
    {
        if (g_previousInitPid.has_value())
        {
            if (g_previousInitPid.value() != Distribution->InitPid)
            {
                g_logfile << "Init's pid is different (" << Distribution->InitPid << " ! = " << g_previousInitPid.value() << ")" << std::endl;
            }
            else
            {
                g_logfile << "Init's pid did not change (" << g_previousInitPid.value() << ")" << std::endl;
                return E_FAIL;
            }
        }
        else
        {
            g_previousInitPid = Distribution->InitPid;
        }
    }
    else if (g_testType == PluginTestType::RunDistroCommand)
    {
        // Launch a process
        std::vector<const char*> arguments = {"/bin/sh", "-c", "cat /etc/issue.net", nullptr};
        wil::unique_socket socket;
        RETURN_IF_FAILED(g_api->ExecuteBinaryInDistribution(Session->SessionId, &Distribution->Id, arguments[0], arguments.data(), &socket));
        g_logfile << "Process created" << std::endl;

        // Validate that the process actually ran inside the distro.
        auto output = ReadFromSocket(socket.get());
        const auto expected = "Debian GNU/Linux 13\n";
        if (std::string(output.begin(), output.end()) != expected)
        {
            g_logfile << "Got unexpected output from bash: " << std::string(output.begin(), output.end())
                      << ", expected: " << expected << std::endl;
            return E_ABORT;
        }

        // Verify that failure to launch a process behaves properly.
        arguments = {"/does-not-exist"};
        g_logfile << "Failed process launch returned:  "
                  << g_api->ExecuteBinaryInDistribution(Session->SessionId, &Distribution->Id, arguments[0], arguments.data(), &socket)
                  << std::endl;

        const GUID guid{};
        g_logfile << "Invalid distro launch returned:  "
                  << g_api->ExecuteBinaryInDistribution(Session->SessionId, &guid, arguments[0], arguments.data(), &socket) << std::endl;
    }
    else if (g_testType == PluginTestType::AsyncApiCall)
    {
        // Validate plugin API calls from a worker thread that outlives the
        // hook. The worker thread is joined in OnDistroStopping — joining is
        // unconditional (no timeout) because letting the worker outlive
        // g_pluginHost (cleared in ~PluginHost) would dereference freed memory.
        g_asyncWorkerOutput.clear();
        g_asyncWorkerResult.emplace();
        g_asyncWorkerFuture = g_asyncWorkerResult->get_future();

        const DWORD sessionId = Session->SessionId;
        const GUID distroId = Distribution->Id;

        g_asyncWorker.emplace([sessionId, distroId]() {
            HRESULT hr = E_FAIL;
            try
            {
                // Sleep briefly so the call is guaranteed to happen after the
                // hook has returned — exercises the cross-apartment callback
                // path from a non-hook thread that hasn't called CoInitializeEx.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                wil::unique_socket socket;
                std::vector<const char*> args = {"/bin/echo", "hello-from-worker", nullptr};
                hr = g_api->ExecuteBinaryInDistribution(sessionId, &distroId, args[0], args.data(), &socket);

                if (SUCCEEDED(hr))
                {
                    const auto output = ReadFromSocket(socket.get());
                    std::string captured(output.begin(), output.end());
                    // Strip trailing newline added by /bin/echo so the log line
                    // doesn't get split when ValidateLogFile splits on '\n'.
                    while (!captured.empty() && (captured.back() == '\n' || captured.back() == '\r'))
                    {
                        captured.pop_back();
                    }
                    std::lock_guard guard{g_logMutex};
                    g_asyncWorkerOutput = std::move(captured);
                }
            }
            catch (...)
            {
                hr = wil::ResultFromCaughtException();
            }

            // Always fulfill the promise so OnDistroStopping's future.get() can
            // never block indefinitely, even if the body above threw.
            g_asyncWorkerResult->set_value(hr);
        });
    }
    else if (g_testType == PluginTestType::CallbackDuringTermination)
    {
        // Validate that callbacks racing VM teardown never crash the service.
        // Workers keep calling into the service across OnDistroStopping /
        // _VmTerminate; each callback runs under (or blocks on) the session's
        // recursive m_instanceLock, so it is naturally serialized against
        // m_utilityVm.reset() and fails gracefully if it lands after teardown.
        // Workers then wind down deterministically (see globals above).
        //
        // Scope: this test exercises only the *happy-path* race — the
        // callback (/bin/true) returns in sub-millisecond, so workers are
        // almost always between iterations when teardown runs. It is *not* a
        // regression test for the hung-callback case, where a service-side
        // callback is stuck inside CreateLinuxProcess waiting on a non-responsive
        // Linux init; that scenario requires termination-event plumbing through
        // WslCoreInstance::CreateLinuxProcess and is tracked separately.
        constexpr int N = 4;

        // Spawn at most once. The post-shutdown StartWsl in
        // CallbacksDuringTerminationDoNotCrash triggers another
        // OnDistroStarted; join the (already wound-down) workers there instead
        // of starting a fresh batch.
        if (g_drainWorkersStarted.exchange(true))
        {
            for (auto& t : g_drainWorkers)
            {
                if (t.joinable())
                {
                    t.join();
                }
            }
            g_drainWorkers.clear();
            return S_OK;
        }

        g_drainSuccess = 0;
        g_drainFailures = 0;
        g_drainWindDown = false;

        const DWORD sessionId = Session->SessionId;
        const GUID distroId = Distribution->Id;

        for (int i = 0; i < N; ++i)
        {
            g_drainWorkers.emplace_back([sessionId, distroId]() {
                int windDownRemaining = -1;
                while (true)
                {
                    wil::unique_socket socket;
                    std::vector<const char*> args = {"/bin/true", nullptr};
                    const HRESULT hr = g_api->ExecuteBinaryInDistribution(sessionId, &distroId, args[0], args.data(), &socket);
                    if (SUCCEEDED(hr))
                    {
                        ++g_drainSuccess;
                    }
                    else
                    {
                        ++g_drainFailures;
                    }

                    if (g_drainWindDown.load())
                    {
                        if (windDownRemaining < 0)
                        {
                            windDownRemaining = c_drainWindDownIterations;
                        }
                        if (windDownRemaining-- == 0)
                        {
                            return;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        }
    }

    return S_OK;
}

HRESULT OnDistroStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    // For AsyncApiCall we defer the "Distribution Stopping" line until after
    // the worker thread has been joined, so the worker's "Async worker output"
    // line is guaranteed to appear before it in the log.
    auto logDistroStopping = [&]() {
        g_logfile << "Distribution Stopping, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
                  << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
                  << ", PidNs=" << Distribution->PidNamespace << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
                  << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;
    };

    if (g_testType == PluginTestType::AsyncApiCall)
    {
        if (g_asyncWorker.has_value())
        {
            // Unconditional join — letting the worker outlive g_pluginHost
            // (cleared in ~PluginHost) would dereference freed memory.
            g_asyncWorker->join();
            g_asyncWorker.reset();

            HRESULT workerHr = S_OK;
            if (g_asyncWorkerFuture.valid())
            {
                workerHr = g_asyncWorkerFuture.get();
                g_asyncWorkerResult.reset();
            }

            if (SUCCEEDED(workerHr))
            {
                LogLine("Async worker output: " + g_asyncWorkerOutput);
            }
            else
            {
                LogLine("Async worker failed: " + std::to_string(workerHr));
            }
        }

        logDistroStopping();
        return S_OK;
    }

    logDistroStopping();

    if (g_testType == PluginTestType::FailToStopDistro)
    {
        g_logfile << "OnDistroStopping: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::SameDistroId && g_distroGuid.has_value())
    {
        if (!IsEqualGUID(g_distroGuid.value(), Distribution->Id))
        {
            g_logfile << "OnDistroStarted: received different GUID" << std::endl;
        }
    }

    return S_OK;
}

HRESULT OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
{
    g_logfile << "Distribution registered, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
              << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
              << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
              << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;

    if (g_testType == PluginTestType::FailToRegisterUnregisterDistro)
    {
        g_logfile << "OnDistributionRegistered: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    return S_OK;
}

HRESULT OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
{
    g_logfile << "Distribution unregistered, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
              << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
              << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
              << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;

    if (g_testType == PluginTestType::FailToRegisterUnregisterDistro)
    {
        g_logfile << "OnDistributionUnregistered: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    return S_OK;
}

HRESULT OnWslcSessionCreated(const WSLCSessionInformation* Session)
try
{
    // ParallelWslcWithCallbacks fires this on multiple notification threads, so route
    // through LogLine (g_logMutex) to keep log lines from interleaving.
    LogLine(std::format(
        "WSLC Session created, name={}, id={}, pid={}, token={}, sid={}",
        wsl::shared::string::WideToMultiByte(Session->DisplayName),
        Session->SessionId,
        Session->ApplicationPid,
        Session->UserToken != nullptr ? "set" : "null",
        Session->UserSid != nullptr ? "set" : "null"));

    if (g_testType == PluginTestType::WslcVmNeverStarted)
    {
        // A plugin call is never a reason to create a VM. This one has to be rejected rather than
        // bringing one up, which the absence of any VM notification in the expected output confirms.
        std::vector<const char*> args = {"/bin/true", nullptr};
        WSLCProcessHandle process = nullptr;
        const auto hr = g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &process, nullptr);
        if (SUCCEEDED(hr))
        {
            g_api->WSLCReleaseProcess(process);
        }

        g_logfile << "WSLC no-vm caller: " << (hr == WSLC_E_VM_NOT_RUNNING ? "rejected" : "unexpected") << std::endl;
        return S_OK;
    }

    if (g_testType == PluginTestType::WslcSessionRejected)
    {
        g_logfile << "OnWslcSessionCreated: ERROR_ACCESS_DENIED" << std::endl;
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }

    if (g_testType == PluginTestType::ParallelWslcWithCallbacks)
    {
        // Spawn a worker that waits for both sessions to arrive (barrier), then
        // makes a callback. Concurrency tracking mirrors ConcurrentApiCalls but
        // across two separate notification calls instead of threads from one hook.
        const auto sessionId = Session->SessionId;
        std::lock_guard<std::mutex> lock(g_parallelWorkersLock);
        g_parallelWorkers.emplace_back([sessionId]() {
            int currentActive = ++g_parallelStarted;

            // Barrier: wait for both workers to spawn before proceeding to callback.
            while (g_parallelStarted.load() < 2)
            {
                std::this_thread::sleep_for(10ms);
            }

            // Track concurrency (same logic as ConcurrentApiCalls).
            currentActive = ++g_parallelSuccess;
            int maxSeen = g_parallelMaxConcurrent.load();
            while (maxSeen < currentActive && !g_parallelMaxConcurrent.compare_exchange_weak(maxSeen, currentActive))
            {
            }

            // Make a callback while both workers are active.
            std::vector<const char*> args = {"/bin/true", nullptr};
            WSLCProcessHandle process = nullptr;
            if (SUCCEEDED(g_api->WSLCCreateProcess(sessionId, args[0], args.data(), nullptr, &process, nullptr)))
            {
                g_api->WSLCReleaseProcess(process);
            }

            --g_parallelSuccess;
        });

        // Return immediately; workers continue in background. They're joined in
        // OnWslcSessionStopping after the second session also completes its hook.
        return S_OK;
    }

    if (g_testType == PluginTestType::WslcAsyncCallbackPostReturn)
    {
        // Spawn a worker that sleeps (to ensure the pump is unregistered), then
        // calls back. Tests the InvokeOnWslPump direct path (no pump registered).
        g_wslcAsyncSessionId = Session->SessionId;
        g_wslcAsyncWorker.emplace([sessionId = Session->SessionId]() {
            // Sleep to ensure OnWslcSessionCreated has returned and the pump
            // is unregistered. This forces the callback through the direct path.
            std::this_thread::sleep_for(200ms);

            std::vector<const char*> args = {"/bin/true", nullptr};
            WSLCProcessHandle process = nullptr;
            const HRESULT hr = g_api->WSLCCreateProcess(sessionId, args[0], args.data(), nullptr, &process, nullptr);
            if (SUCCEEDED(hr))
            {
                g_api->WSLCReleaseProcess(process);
                LogLine("Async callback (post-notification): success");
            }
            else
            {
                LogLine("Async callback (post-notification): failed " + std::to_string(hr));
            }
        });

        return S_OK;
    }

    return S_OK;
}
CATCH_RETURN();

// These checks need a running VM, and a plugin call never creates one, so they run from the VM-started
// hook rather than from session creation.
void RunWslcSuccessChecks(const WSLCSessionInformation* Session)
{
    {
        // Helper: run a command in the root namespace and return (status, stdout, stderr).
        auto runCommand = [&](const char* cmd,
                              const std::optional<std::string>& input = {},
                              std::vector<const char*> env = {}) -> std::tuple<int, std::string, std::string> {
            std::vector<const char*> arguments = {"/bin/sh", "-c", cmd, nullptr};
            WSLCProcessHandle process = nullptr;
            THROW_IF_FAILED(g_api->WSLCCreateProcess(
                Session->SessionId, arguments[0], arguments.data(), env.empty() ? nullptr : env.data(), &process, nullptr));
            auto releaseProcess = wil::scope_exit([&]() { g_api->WSLCReleaseProcess(process); });

            wil::unique_socket stdinSocket;
            wil::unique_socket stdoutSocket;
            wil::unique_socket stderrSocket;
            wil::unique_handle exitEvent;
            THROW_IF_FAILED(g_api->WSLCProcessGetFd(process, WSLCProcessFdStdin, &stdinSocket));
            THROW_IF_FAILED(g_api->WSLCProcessGetFd(process, WSLCProcessFdStdout, &stdoutSocket));
            THROW_IF_FAILED(g_api->WSLCProcessGetFd(process, WSLCProcessFdStderr, &stderrSocket));
            THROW_IF_FAILED(g_api->WSLCProcessGetExitEvent(process, &exitEvent));

            const auto validateSocket = [](SOCKET socket) {
                int socketType = 0;
                int socketTypeSize = sizeof(socketType);
                if (getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&socketType), &socketTypeSize) == SOCKET_ERROR)
                {
                    THROW_HR(HRESULT_FROM_WIN32(WSAGetLastError()));
                }

                THROW_HR_IF(E_UNEXPECTED, socketType != SOCK_STREAM);
            };

            validateSocket(stdinSocket.get());
            validateSocket(stdoutSocket.get());
            validateSocket(stderrSocket.get());

            std::string out;
            std::string err;

            MultiHandleWait io;
            io.AddHandle(std::make_unique<ReadHandle>(
                std::move(stdoutSocket), [&out](const auto& span) { out.append(span.begin(), span.end()); }));

            io.AddHandle(std::make_unique<ReadHandle>(
                std::move(stderrSocket), [&err](const auto& span) { err.append(span.begin(), span.end()); }));

            io.AddHandle(std::make_unique<EventHandle>(std::move(exitEvent)));

            if (input.has_value())
            {
                io.AddHandle(std::make_unique<WriteHandle>(std::move(stdinSocket), std::vector<char>(input->begin(), input->end())));
            }
            else
            {
                stdinSocket.reset();
            }

            io.Run(60000ms);

            int status = 0;
            THROW_IF_FAILED(g_api->WSLCProcessGetExitCode(process, &status));
            g_logfile << "Command: '" << cmd << "', status=" << status << ", stdout: " << out << ", stderr: " << err << std::endl;

            return {status, out, err};
        };

        // Test process creation (output & exit code validated by the test code).
        {
            runCommand("echo -n stdout-ok && echo -n stderr-ok >&2");
            runCommand("cat", "stdin-ok");
            runCommand("exit 12");
            runCommand("echo -n $ENV", {}, {"ENV=env-ok", nullptr});
        }

        // Validate that trying to execute a non-existent file fails with the expected error code.
        {
            WSLCProcessHandle process = nullptr;
            int errnoValue = 0;
            std::vector<const char*> args = {"does-not-exist", nullptr};

            auto hr = g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &process, &errnoValue);
            g_logfile << "WSLCCreateProcess(does-not-exist): " << std::hex << hr << ", errno=" << std::dec << errnoValue << std::endl;
        }

        // Validate various error paths
        {
            std::vector<const char*> args = {"/bin/sh", "-c", "sleep 9999", nullptr};
            WSLCProcessHandle process = nullptr;
            THROW_IF_FAILED(g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &process, nullptr));
            auto releaseProcess = wil::scope_exit([&]() { g_api->WSLCReleaseProcess(process); });

            // Validate that getting an fd that doesn't exist fails with the expected error code.
            SOCKET dummy = INVALID_SOCKET;
            g_logfile << "WSLCProcessGetFd(999): " << g_api->WSLCProcessGetFd(process, static_cast<WSLCProcessFd>(999), &dummy) << std::endl;
            int exitCode = -1;

            g_logfile << "WSLCProcessGetExitCode(<running>): " << g_api->WSLCProcessGetExitCode(process, &exitCode) << std::endl;
        }

        const auto testFolder = L"C:\\";
        constexpr auto testFileName = L"plugin-test.txt";
        constexpr auto rwMountpoint = "/mnt/wsl-plugin/plugin-rw-test";
        constexpr auto roMountpoint = "/mnt/wsl-plugin/plugin-ro-test";

        // Validate rw mounts.
        {
            auto rwCleanup = wil::scope_exit_log(
                WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove(std::wstring(testFolder) + testFileName); });

            {
                std::ofstream file(std::wstring(testFolder) + testFileName);
                file << "Windows-content";
            }

            const auto deniedFilePath = std::wstring(testFolder) + L"plugin-denied.txt";
            {
                wil::unique_hfile deniedFile{
                    CreateFileW(deniedFilePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
                THROW_LAST_ERROR_IF(!deniedFile);
            }

            auto deniedFileCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove(deniedFilePath); });

            PACL originalAcl = nullptr;
            wil::unique_hlocal originalDescriptor;
            THROW_IF_WIN32_ERROR(GetNamedSecurityInfoW(
                deniedFilePath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &originalAcl, nullptr, &originalDescriptor));

            auto restoreAcl = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                THROW_IF_WIN32_ERROR(SetNamedSecurityInfoW(
                    const_cast<LPWSTR>(deniedFilePath.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, originalAcl, nullptr));
            });

            EXPLICIT_ACCESSW deniedAccess{};
            deniedAccess.grfAccessPermissions = FILE_READ_DATA;
            deniedAccess.grfAccessMode = DENY_ACCESS;
            deniedAccess.grfInheritance = NO_INHERITANCE;
            deniedAccess.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            deniedAccess.Trustee.ptstrName = static_cast<LPWSTR>(Session->UserSid);

            wsl::windows::common::security::unique_acl deniedAcl;
            THROW_IF_WIN32_ERROR(SetEntriesInAclW(1, &deniedAccess, originalAcl, &deniedAcl));
            THROW_IF_WIN32_ERROR(SetNamedSecurityInfoW(
                const_cast<LPWSTR>(deniedFilePath.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, deniedAcl.get(), nullptr));

            {
                wil::unique_handle impersonationToken;
                THROW_LAST_ERROR_IF(!DuplicateTokenEx(
                    Session->UserToken, TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr, SecurityImpersonation, TokenImpersonation, &impersonationToken));
                auto revert = wil::impersonate_token(impersonationToken.get());
                wil::unique_hfile deniedFile{
                    CreateFileW(deniedFilePath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
                const auto openError = GetLastError();
                THROW_HR_IF(E_UNEXPECTED, deniedFile || openError != ERROR_ACCESS_DENIED);
            }

            // Mount read-write and verify the file can be read from Linux.
            THROW_IF_FAILED(g_api->WSLCMountFolder(Session->SessionId, testFolder, rwMountpoint, false));

            g_logfile << "WSLC RW folder mounted at: " << rwMountpoint << std::endl;

            auto readCmd = std::format("cat {}/{}", rwMountpoint, testFileName);
            runCommand(readCmd.c_str());

            auto deniedReadCmd = std::format("cat {}/plugin-denied.txt", rwMountpoint);
            runCommand(deniedReadCmd.c_str());

            THROW_IF_FAILED(g_api->WSLCUnmountFolder(Session->SessionId, rwMountpoint));
        }

        // Validate ro mounts.
        {
            THROW_IF_FAILED(g_api->WSLCMountFolder(Session->SessionId, L"C:\\", roMountpoint, TRUE));

            g_logfile << "WSLC RO folder mounted at: " << roMountpoint << std::endl;

            // Attempt to write from Linux — should fail on a read-only mount.
            auto writeCmd = std::format("echo fail > {}/should-not-exist.txt", roMountpoint);
            runCommand(writeCmd.c_str());

            THROW_IF_FAILED(g_api->WSLCUnmountFolder(Session->SessionId, roMountpoint));
        }

        // Validate that trying to mount a folder that doesn't exist fails with the expected error code.
        g_logfile << "WSLCMountFolder(nonexistent): " << g_api->WSLCMountFolder(Session->SessionId, L"C:\\nonexistent", roMountpoint, TRUE)
                  << std::endl;

        // Validate that non-absolute mountpoints are rejected.
        g_logfile << "WSLCMountFolder(relative): " << g_api->WSLCMountFolder(Session->SessionId, L"C:\\", "relative-mountpoint", TRUE)
                  << std::endl;

        g_logfile << "Test completed" << std::endl;
    }
}

HRESULT OnWslcSessionStopping(const WSLCSessionInformation* Session)
{
    // Drain the stop-window thread first, then report what it observed. Logging from here rather than
    // from that thread keeps the log single-writer and the expected output deterministic. Safe
    // because this is the last event of the session, so the join cannot run inside a VM notification.
    if (g_stopWindowCaller.joinable())
    {
        g_stopWindowCaller.join();

        g_logfile << "WSLC stop-window caller: " << (SUCCEEDED(g_stopWindowCallerResult) ? "ok" : "failed") << std::endl;
        g_logfile << "WSLC leaked process died: " << (g_leakedProcessDied.load() ? "yes" : "no") << std::endl;
    }

    // Close the duplicated exit event if the stop-window thread did not get far enough to claim it.
    // The leaked process wrapper is deliberately not released: it holds a COM proxy marshalled to the
    // OnWslcVmStopping callback's thread, and releasing it from this one risks the same
    // RPC_E_WRONG_THREAD hazard that forced the exit event to be cached as a plain handle. It is one
    // wrapper for the lifetime of a test process, so leaking it is the safer trade.
    if (auto* exitEvent = g_leakedProcessExitEvent.exchange(nullptr); exitEvent != nullptr)
    {
        const wil::unique_handle owned{exitEvent};
    }

    // Join parallel workers after both sessions have been created.
    if (g_testType == PluginTestType::ParallelWslcWithCallbacks)
    {
        std::lock_guard<std::mutex> lock(g_parallelWorkersLock);
        if (!g_parallelWorkers.empty())
        {
            for (auto& t : g_parallelWorkers)
            {
                if (t.joinable())
                {
                    t.join();
                }
            }

            // Log summary only once (after second session stops).
            static bool logged = false;
            if (!logged)
            {
                LogLine(
                    "Parallel callbacks complete: success=" + std::to_string(g_parallelWorkers.size()) +
                    " maxConcurrent=" + std::to_string(g_parallelMaxConcurrent.load()));
                logged = true;
            }
        }
    }

    // Join async worker (spawned in OnWslcSessionCreated, runs post-return).
    if (g_testType == PluginTestType::WslcAsyncCallbackPostReturn && g_wslcAsyncWorker.has_value())
    {
        if (g_wslcAsyncWorker->joinable())
        {
            g_wslcAsyncWorker->join();
        }
        g_wslcAsyncWorker.reset();
    }

    // Can be invoked on parallel notification threads; serialize via LogLine.
    LogLine(std::format(
        "WSLC Session stopping, name={}, id={}", wsl::shared::string::WideToMultiByte(Session->DisplayName), Session->SessionId));

    return S_OK;
}

HRESULT OnWslcContainerStarted(const WSLCSessionInformation* Session, LPCSTR InspectJson)
try
{
    auto container = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectContainer>(InspectJson);

    g_logfile << "WSLC Container started, session=" << Session->SessionId << ", id=" << container.Id << ", name=" << container.Name
              << ", image=" << container.Config.Image << ", state=" << container.State.Status << std::endl;

    if (g_testType == PluginTestType::WslcContainerRejected)
    {
        g_logfile << "OnWslcContainerStarted: ERROR_ACCESS_DENIED" << std::endl;
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT OnWslcContainerStopping(const WSLCSessionInformation* Session, LPCSTR ContainerId)
{
    g_logfile << "WSLC Container stopping, session=" << Session->SessionId << ", id=" << ContainerId << std::endl;
    return S_OK;
}

HRESULT OnWslcImageCreated(const WSLCSessionInformation* Session, LPCSTR InspectJson)
{
    auto image = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectImage>(InspectJson);
    auto name = (image.RepoTags.has_value() && !image.RepoTags->empty()) ? image.RepoTags->front() : "<none>";
    g_logfile << "WSLC Image created, session=" << Session->SessionId << ", id=" << image.Id << ", name=" << name << std::endl;
    return S_OK;
}

HRESULT OnWslcImageDeleted(const WSLCSessionInformation* Session, LPCSTR ImageId)
{
    g_logfile << "WSLC Image deleted, session=" << Session->SessionId << ", id=" << ImageId << std::endl;
    return S_OK;
}

HRESULT OnWslcVmStarted(const WSLCSessionInformation* Session)
try
{
    if (g_testType == PluginTestType::WslcSuccess)
    {
        // Run once: the checks are written against the first VM of the session.
        static std::atomic<bool> done = false;
        if (!done.exchange(true))
        {
            RunWslcSuccessChecks(Session);
        }

        return S_OK;
    }

    if (g_testType == PluginTestType::WslcVmStopCommitted)
    {
        g_logfile << "WSLC VM started, session=" << Session->SessionId << std::endl;
        return S_OK;
    }

    // The VM-never-started test expects no VM hook to fire at all. Logging here is the diagnostic
    // that makes a regression visible: any line from this hook fails the expected output.
    if (g_testType == PluginTestType::WslcVmNeverStarted)
    {
        g_logfile << "WSLC VM started, session=" << Session->SessionId << std::endl;
        return S_OK;
    }

    // Only log/exercise for the dedicated VM-restart test so other WSLC plugin tests (which start
    // and stop VMs incidentally) are not affected by extra log lines.
    if (g_testType != PluginTestType::WslcVmRestart)
    {
        return S_OK;
    }

    g_logfile << "WSLC VM started, session=" << Session->SessionId << std::endl;

    // Prove the VM is usable from within the started hook, and that calling back into the session
    // (WSLCCreateProcess acquires a VM lease + the runtime lock) does not deadlock.
    std::vector<const char*> args = {"/bin/true", nullptr};
    WSLCProcessHandle process = nullptr;
    const auto hr = g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &process, nullptr);
    g_logfile << "WSLC VM started reentrant WSLCCreateProcess: " << (SUCCEEDED(hr) ? "ok" : "failed") << std::endl;
    if (SUCCEEDED(hr))
    {
        g_api->WSLCReleaseProcess(process);
    }

    // Also exercise a reentrant mount + unmount from the started hook; the session is alive here so
    // both calls succeed, validating that mount management reentrant from OnVmStarted does not deadlock.
    constexpr auto* mountpoint = "/test-plugin/vm-started-mount";
    const auto mountHr = g_api->WSLCMountFolder(Session->SessionId, L"C:\\", mountpoint, TRUE);
    if (SUCCEEDED(mountHr))
    {
        const auto unmountHr = g_api->WSLCUnmountFolder(Session->SessionId, mountpoint);
        g_logfile << "WSLC VM started mount+unmount: " << (SUCCEEDED(unmountHr) ? "ok" : "failed") << std::endl;
    }
    else
    {
        g_logfile << "WSLC VM started mount+unmount: skipped" << std::endl;
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT OnWslcVmStopping(const WSLCSessionInformation* Session)
try
{
    if (g_testType == PluginTestType::WslcVmNeverStarted)
    {
        g_logfile << "WSLC VM stopping, session=" << Session->SessionId << std::endl;
        return S_OK;
    }

    if (g_testType == PluginTestType::WslcVmStopCommitted)
    {
        // Only the idle teardown is interesting here. The session's final teardown races with the
        // session-stopping notification, which is delivered independently, so logging it would make
        // the expected output order-dependent on that race.
        if (g_stopWindowCaller.joinable())
        {
            return S_OK;
        }

        g_logfile << "WSLC VM stopping, session=" << Session->SessionId << std::endl;

        // Deliberately leave a live process behind when this callback returns. The stop is committed
        // before it is announced, so the VM goes away regardless and this process dies with it --
        // which is exactly what the callback was just told would happen.
        //
        // Created before the thread below is started so its exit event is published first: that thread
        // claims the event and must not race ahead of it.
        std::vector<const char*> args = {"/bin/sleep", "60", nullptr};
        WSLCProcessHandle leaked = nullptr;
        const auto hr = g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &leaked, nullptr);
        g_leakedProcess.store(leaked);

        // Cache the exit event while still on the thread the process proxy is marshalled to.
        if (SUCCEEDED(hr))
        {
            HANDLE exitEvent = nullptr;
            if (SUCCEEDED(g_api->WSLCProcessGetExitEvent(leaked, &exitEvent)))
            {
                g_leakedProcessExitEvent.store(exitEvent);
            }
        }

        g_logfile << "WSLC VM stopping leaked process: " << (SUCCEEDED(hr) ? "ok" : "failed") << std::endl;

        // A call from a thread this plugin owns is served on the same terms as the callback itself:
        // by the VM that is stopping, not by a future one and not by a new one. It must not block on
        // the teardown. Results are logged when this thread is joined, so the output stays deterministic.
        const auto sessionId = Session->SessionId;
        g_stopWindowCaller = std::thread([sessionId]() {
            std::vector<const char*> processArgs = {"/bin/true", nullptr};
            WSLCProcessHandle process = nullptr;
            g_stopWindowCallerResult = g_api->WSLCCreateProcess(sessionId, processArgs[0], processArgs.data(), nullptr, &process, nullptr);
            if (SUCCEEDED(g_stopWindowCallerResult))
            {
                g_api->WSLCReleaseProcess(process);
            }

            // The announced stop takes the VM away and the leaked process with it. Prove that
            // directly instead of inferring it from the fact that a new VM started -- a process that
            // outlived the stop is the exact symptom of an announced stop that did not happen.
            if (auto* exitEvent = g_leakedProcessExitEvent.exchange(nullptr); exitEvent != nullptr)
            {
                // GetExitEvent is marshalled as an [out, system_handle(sh_event)] parameter, so this
                // is a duplicate owned by this process.
                const wil::unique_handle owned{exitEvent};
                g_leakedProcessDied = WaitForSingleObject(owned.get(), 30 * 1000) == WAIT_OBJECT_0;
            }
        });

        // Give the thread time to issue its call inside the stop window. If it has not, the test still
        // passes -- it just proves less.
        std::this_thread::sleep_for(500ms);

        return S_OK;
    }

    if (g_testType != PluginTestType::WslcVmRestart)
    {
        return S_OK;
    }

    g_logfile << "WSLC VM stopping, session=" << Session->SessionId << std::endl;

    // Proves OnVmStopping doesn't deadlock a plugin that calls back in: on idle teardown these are
    // served by the VM that is still stopping and succeed; on permanent teardown they fail cleanly.
    std::vector<const char*> args = {"/bin/true", nullptr};
    WSLCProcessHandle process = nullptr;
    const auto processHr = g_api->WSLCCreateProcess(Session->SessionId, args[0], args.data(), nullptr, &process, nullptr);
    g_logfile << "WSLC VM stopping reentrant WSLCCreateProcess: " << (SUCCEEDED(processHr) ? "ok" : "failed") << std::endl;
    if (SUCCEEDED(processHr))
    {
        g_api->WSLCReleaseProcess(process);
    }

    constexpr auto* mountpoint = "/test-plugin/vm-stopping-mount";
    const auto mountHr = g_api->WSLCMountFolder(Session->SessionId, L"C:\\", mountpoint, TRUE);
    if (SUCCEEDED(mountHr))
    {
        const auto unmountHr = g_api->WSLCUnmountFolder(Session->SessionId, mountpoint);
        g_logfile << "WSLC VM stopping mount+unmount: " << (SUCCEEDED(unmountHr) ? "ok" : "failed") << std::endl;
    }
    else
    {
        g_logfile << "WSLC VM stopping mount+unmount: skipped" << std::endl;
    }

    return S_OK;
}
CATCH_RETURN();

EXTERN_C __declspec(dllexport) HRESULT WSLPLUGINAPI_ENTRYPOINTV1(const WSLPluginAPIV1* Api, WSLPluginHooksV1* Hooks)
{
    try
    {
        const auto key = OpenTestRegistryKey(KEY_READ);

        const std::wstring outputFile = ReadString(key.get(), nullptr, c_logFile);
        g_logfile.open(outputFile);
        THROW_HR_IF(E_UNEXPECTED, !g_logfile);

        g_testType = static_cast<PluginTestType>(ReadDword(key.get(), nullptr, c_testType, static_cast<DWORD>(PluginTestType::Invalid)));
        THROW_HR_IF(
            E_INVALIDARG,
            static_cast<DWORD>(g_testType) <= 0 || static_cast<DWORD>(g_testType) > static_cast<DWORD>(PluginTestType::WslcAsyncCallbackPostReturn));

        g_logfile << "Plugin loaded. TestMode=" << static_cast<DWORD>(g_testType) << std::endl;
        g_api = Api;
        Hooks->OnVMStarted = &OnVmStarted;
        Hooks->OnVMStopping = &OnVmStopping;
        Hooks->OnDistributionStarted = &OnDistroStarted;
        Hooks->OnDistributionStopping = &OnDistroStopping;
        Hooks->OnDistributionRegistered = &OnDistributionRegistered;
        Hooks->OnDistributionUnregistered = &OnDistributionUnregistered;
        Hooks->OnSessionCreated = &OnWslcSessionCreated;
        Hooks->OnSessionStopping = &OnWslcSessionStopping;
        Hooks->ContainerStarted = &OnWslcContainerStarted;
        Hooks->ContainerStopping = &OnWslcContainerStopping;
        Hooks->ImageCreated = &OnWslcImageCreated;
        Hooks->ImageDeleted = &OnWslcImageDeleted;
        Hooks->WslcVmStarted = &OnWslcVmStarted;
        Hooks->WslcVmStopping = &OnWslcVmStopping;

        if (g_testType == PluginTestType::FailToLoad)
        {
            g_logfile << "OnLoad: E_UNEXPECTED" << std::endl;
            return E_UNEXPECTED;
        }
        else if (g_testType == PluginTestType::PluginRequiresUpdate)
        {
            g_logfile << "OnLoad: WSL_E_PLUGINREQUIRESUPDATE" << std::endl;

            WSL_PLUGIN_REQUIRE_VERSION(9999, 99, 99, Api);
        }
    }
    catch (...)
    {
        const auto error = wil::ResultFromCaughtException();
        if (g_logfile)
        {
            g_logfile << "Failed to initialize plugin, " << error << std::endl;
        }

        return error;
    }
    return S_OK;
}
