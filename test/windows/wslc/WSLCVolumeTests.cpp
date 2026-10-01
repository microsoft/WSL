/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVolumeTests.cpp

Abstract:

    This file contains test cases for the WSLC named volume API.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCVolumeTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCVolumeTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    WSLC_TEST_METHOD(VhdFormatting)
    {
        constexpr auto formatedVhd = L"test-format-vhd.vhdx";

        // TODO: Replace this by a proper SDK method once it exists
        auto tokenInfo = wil::get_token_information<TOKEN_USER>();
        wsl::core::filesystem::CreateVhd(formatedVhd, 100 * 1024 * 1024, tokenInfo->User.Sid, false, false);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(formatedVhd)); });

        // Format the disk.
        auto absoluteVhdPath = std::filesystem::absolute(formatedVhd).wstring();
        VERIFY_SUCCEEDED(m_defaultSession->FormatVirtualDisk(absoluteVhdPath.c_str()));

        // Validate error paths.
        VERIFY_ARE_EQUAL(m_defaultSession->FormatVirtualDisk(L"DoesNotExist.vhdx"), E_INVALIDARG);
        VERIFY_ARE_EQUAL(m_defaultSession->FormatVirtualDisk(L"C:\\DoesNotExist.vhdx"), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }

    // Exercises behavior that all volume drivers must implement identically:
    // create, duplicate-name rejection, multi-mount, cross-container read/write,
    // in-use deletion rejection, and clean deletion after the referencing container is removed.
    void ValidateNamedVolumeContract(std::string_view driver, const WSLCDriverOption* driverOpts, ULONG driverOptsCount)
    {
        const std::string driverStr(driver);
        const std::string volumeName = std::format("wslc-test-named-volume-{}", driver);

        // Best-effort cleanup in case of leftovers from a previous failed run.
        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = driverStr.c_str();
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = driverOptsCount;

        // Create volume and validate duplicate volume name handling.
        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));
        VERIFY_ARE_EQUAL(std::string(volInfo.Name), volumeName);
        VERIFY_ARE_EQUAL(std::string(volInfo.Driver), driverStr);
        VERIFY_ARE_EQUAL(m_defaultSession->CreateVolume(&volumeOptions, &volInfo), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

        // Verify the same named volume can be mounted more than once with different container paths.
        {
            WSLCContainerLauncher duplicateNamedVolumes(
                "debian:latest",
                std::format("named-volume-dup-{}", driver),
                {"/bin/sh", "-c", "echo duplicated >/data-a/dup.txt ; cat /data-b/dup.txt"});
            duplicateNamedVolumes.AddNamedVolume(volumeName, "/data-a", false);
            duplicateNamedVolumes.AddNamedVolume(volumeName, "/data-b", true);

            auto duplicateNamedVolumesContainer = duplicateNamedVolumes.Launch(*m_defaultSession);
            auto duplicateNamedVolumesProcess = duplicateNamedVolumesContainer.GetInitProcess();
            ValidateProcessOutput(duplicateNamedVolumesProcess, {{1, "duplicated\n"}});
        }

        // Verify CreateContainer with named volume mounts the volume into the container.
        {
            WSLCContainerLauncher writer(
                "debian:latest",
                std::format("named-volume-writer-{}", driver),
                {"/bin/sh", "-c", "echo wslc-named-volume >/data/marker.txt"});
            writer.AddNamedVolume(volumeName, "/data", false);

            auto writerContainer = writer.Launch(*m_defaultSession);
            auto writerProcess = writerContainer.GetInitProcess();
            ValidateProcessOutput(writerProcess, {});

            WSLCContainerLauncher reader(
                "debian:latest", std::format("named-volume-reader-{}", driver), {"/bin/sh", "-c", "cat /data/marker.txt"});
            reader.AddNamedVolume(volumeName, "/data", true);

            auto readerContainer = reader.Launch(*m_defaultSession);
            auto readerProcess = readerContainer.GetInitProcess();
            ValidateProcessOutput(readerProcess, {{1, "wslc-named-volume\n"}});
        }

        // Verify we cannot delete a named volume while a container references it.
        WSLCContainerLauncher holder("debian:latest", std::format("named-volume-holder-{}", driver), {"sleep", "99999"});
        holder.AddNamedVolume(volumeName, "/data", false);

        auto [holderCreateResult, holderContainerResult] = holder.CreateNoThrow(*m_defaultSession);
        VERIFY_SUCCEEDED(holderCreateResult);
        VERIFY_IS_TRUE(holderContainerResult.has_value());

        auto holderContainer = std::move(holderContainerResult.value());
        holderContainer.SetDeleteOnClose(false);

        VERIFY_ARE_EQUAL(m_defaultSession->DeleteVolume(volumeName.c_str()), HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION));

        // Verify that after deleting the container, the volume can be deleted.
        VERIFY_SUCCEEDED(holderContainer.Get().Delete(WSLCDeleteFlagsNone));
        VERIFY_SUCCEEDED(m_defaultSession->DeleteVolume(volumeName.c_str()));

        cleanup.release();
    }

    WSLC_TEST_METHOD(NamedVolumesVhd)
    {
        WSLCDriverOption driverOpts[] = {{"SizeBytes", "1073741824"}};
        ValidateNamedVolumeContract("vhd", driverOpts, ARRAYSIZE(driverOpts));

        // VHD-driver-specific: validate the host-side .vhdx artifact and the
        // /mnt/wslc-volumes ext4 mount inside the VM appear and disappear with
        // the volume.
        const std::string volumeName = "wslc-test-named-volume-vhd-host";
        const std::filesystem::path volumeVhdPath = m_storagePath / "volumes" / (volumeName + ".vhdx");

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = "vhd";
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        VERIFY_IS_TRUE(std::filesystem::exists(volumeVhdPath));
        ExpectMount(m_defaultSession.get(), std::format("/mnt/wslc-volumes/{}", volumeName), std::optional<std::string>{"*ext4*"});

        VERIFY_SUCCEEDED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        cleanup.release();

        ExpectMount(m_defaultSession.get(), std::format("/mnt/wslc-volumes/{}", volumeName), std::nullopt);
        VERIFY_IS_FALSE(std::filesystem::exists(volumeVhdPath));
    }

    WSLC_TEST_METHOD(NamedVolumesVhdSeedsImageData)
    {
        // A freshly formatted VHD volume must be seeded with the image's content
        // on first use, just like a guest volume. mkfs.ext4 creates a lost+found
        // directory at the volume root; if it isn't removed, Docker treats the
        // volume as non-empty and skips the copy-up that seeds image data.
        // Mounting the empty volume over a directory the image is guaranteed to
        // populate (/etc) exercises that copy-up.
        WSLCDriverOption driverOpts[] = {{"SizeBytes", "1073741824"}};
        const std::string volumeName = "wslc-test-named-volume-vhd-seed";

        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = "vhd";
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "wslc-vhd-seed-container", {"/bin/sh", "-c", "ls -A /etc"});
        launcher.AddNamedVolume(volumeName, "/etc", false);

        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);

        // Image content was seeded into the volume...
        VERIFY_IS_TRUE(
            result.Output[1].find("passwd") != std::string::npos,
            L"Image's /etc content should be seeded into the fresh VHD volume");

        // ...and the ext4 lost+found is gone, so it never blocked copy-up.
        VERIFY_IS_TRUE(
            result.Output[1].find("lost+found") == std::string::npos, L"lost+found should have been removed from the volume root");
    }

    WSLC_TEST_METHOD(NamedVolumesGuest)
    {
        ValidateNamedVolumeContract("guest", nullptr, 0);
    }

    WSLC_TEST_METHOD(NamedVolumesStress)
    {
        constexpr unsigned int c_threadCount = 8;
        constexpr unsigned int c_iterationsPerThread = 50;
        const std::string volumeName = "wslc-stress-vol";

        // Best-effort cleanup of any leftover volume from prior runs / on test exit.
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        std::atomic<unsigned int> failures = 0;
        std::vector<std::thread> threads;
        threads.reserve(c_threadCount);

        for (unsigned int t = 0; t < c_threadCount; ++t)
        {
            threads.emplace_back([&]() {
                for (unsigned int i = 0; i < c_iterationsPerThread; ++i)
                {
                    WSLCVolumeOptions volumeOptions{};
                    volumeOptions.Name = volumeName.c_str();
                    volumeOptions.Driver = "guest";

                    WSLCVolumeInformation volInfo{};
                    HRESULT hrCreate = m_defaultSession->CreateVolume(&volumeOptions, &volInfo);
                    if (FAILED(hrCreate) && hrCreate != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
                    {
                        LogError("CreateVolume(%hs) unexpected HR: 0x%08x", volumeName.c_str(), hrCreate);
                        ++failures;
                    }

                    HRESULT hrDelete = m_defaultSession->DeleteVolume(volumeName.c_str());
                    if (FAILED(hrDelete) && hrDelete != WSLC_E_VOLUME_NOT_FOUND)
                    {
                        LogError("DeleteVolume(%hs) unexpected HR: 0x%08x", volumeName.c_str(), hrDelete);
                        ++failures;
                    }
                }
            });
        }

        for (auto& thread : threads)
        {
            thread.join();
        }

        VERIFY_ARE_EQUAL(failures.load(), 0u);

        // Every thread's iteration ends with a Delete, so the globally-last operation across
        // all threads is guaranteed to be a Delete. The volume must therefore not exist in
        // either our cache or docker -- if either disagrees, our state is desynced from docker.

        // Our cache view: InspectVolume must report not-found.
        wil::unique_cotaskmem_ansistring inspectOutput;
        VERIFY_ARE_EQUAL(m_defaultSession->InspectVolume(volumeName.c_str(), &inspectOutput), WSLC_E_VOLUME_NOT_FOUND);

        // Docker's view: `docker volume inspect` must also report not-found (non-zero exit).
        ExpectCommandResult(m_defaultSession.get(), {"/usr/bin/docker", "volume", "inspect", volumeName}, 1);
    }

    // Verifies that a container using a named volume survives a session restart and the volume's data is preserved.
    void ValidateNamedVolumeRecoveryContract(std::string_view driver, const WSLCDriverOption* driverOpts, ULONG driverOptsCount)
    {
        const std::string driverStr(driver);
        const std::string volumeName = std::format("wslc-test-named-volume-{}", driver);
        const std::string containerName = std::format("wslc-test-container-{}", driver);

        // Best-effort cleanup in case prior failed runs left artifacts behind.
        RunCommand(m_defaultSession.get(), {"/usr/bin/docker", "rm", "-f", containerName});
        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

        auto cleanup = wil::scope_exit([&]() {
            RunCommand(m_defaultSession.get(), {"/usr/bin/docker", "rm", "-f", containerName});
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        });

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = driverStr.c_str();
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = driverOptsCount;

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));

        // Create a container that uses the named volume and writes a marker.
        {
            WSLCContainerLauncher writer(
                "debian:latest", containerName, {"/bin/sh", "-c", "echo named-volume-recovery >/data/marker.txt"});
            writer.AddNamedVolume(volumeName, "/data", false);

            auto writerContainer = writer.Launch(*m_defaultSession);
            writerContainer.SetDeleteOnClose(false);

            auto writerProcess = writerContainer.GetInitProcess();
            ValidateProcessOutput(writerProcess, {});
        }

        // Restart the session and verify the container is recovered.
        ResetTestSession();

        auto recoveredContainer = OpenContainer(m_defaultSession.get(), containerName);
        recoveredContainer.SetDeleteOnClose(false);

        // Verify the named volume still contains the marker after restart.
        {
            WSLCContainerLauncher reader(
                "debian:latest", std::format("{}-reader", containerName), {"/bin/sh", "-c", "cat /data/marker.txt"});
            reader.AddNamedVolume(volumeName, "/data", true);

            auto readerContainer = reader.Launch(*m_defaultSession);
            auto readerProcess = readerContainer.GetInitProcess();
            ValidateProcessOutput(readerProcess, {{1, "named-volume-recovery\n"}});
        }
    }

    WSLC_TEST_METHOD(NamedVolumeRecovery)
    {
        ValidateNamedVolumeRecoveryContract("guest", nullptr, 0);
    }

    WSLC_TEST_METHOD(NamedVolumesVhdSessionRecovery)
    {

        WSLCDriverOption driverOpts[] = {{"SizeBytes", "1073741824"}};
        ValidateNamedVolumeRecoveryContract("vhd", driverOpts, ARRAYSIZE(driverOpts));

        // Re-create the volume (the recovery helper cleans up on exit) so we
        // can test the "delete VHD while session is down" scenario.
        const std::string volumeName = "wslc-test-named-volume-vhd";
        const std::string containerName = "wslc-test-container-vhd";

        // Prune containers on exit so this test doesn't leak "wslc-test-container-vhd" on exit.
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            PruneResult result;
            LOG_IF_FAILED(m_defaultSession->PruneContainers(nullptr, 0, &result.result));
        });

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = "vhd";
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));

        // Create a container that depends on the volume so we can verify it
        // gets dropped when the backing .vhdx is removed.
        {
            WSLCContainerLauncher writer("debian:latest", containerName, {"/bin/sh", "-c", "echo vhd-recovery >/data/marker.txt"});
            writer.AddNamedVolume(volumeName, "/data", false);

            auto writerContainer = writer.Launch(*m_defaultSession);
            writerContainer.SetDeleteOnClose(false);

            auto writerProcess = writerContainer.GetInitProcess();
            ValidateProcessOutput(writerProcess, {});
        }

        const std::filesystem::path volumeVhdPath = m_storagePath / "volumes" / (volumeName + ".vhdx");

        {
            auto restartSession = ResetTestSession();

            VERIFY_IS_TRUE(std::filesystem::exists(volumeVhdPath));

            std::error_code error;
            VERIFY_IS_TRUE(std::filesystem::remove(volumeVhdPath, error));
            VERIFY_ARE_EQUAL(error, std::error_code{});
        }

        // The container can still be opened even though its backing volume is gone, so the
        // user is able to inspect and delete it.
        wil::com_ptr<IWSLCContainer> recoveredContainer;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(containerName.c_str(), &recoveredContainer));

        // Starting it must fail since the referenced volume cannot be brought online.
        VERIFY_ARE_EQUAL(recoveredContainer->Start(WSLCContainerStartFlagsNone, nullptr, nullptr), WSLC_E_VOLUME_NOT_AVAILABLE);
        ValidateCOMErrorMessageContains(wsl::shared::string::MultiByteToWide(volumeName));

        // The container is not running, so the restart is only its start phase and is refused the same way.
        VERIFY_ARE_EQUAL(recoveredContainer->Restart(WSLCSignalSIGTERM, 0, nullptr), WSLC_E_VOLUME_NOT_AVAILABLE);
        ValidateCOMErrorMessageContains(wsl::shared::string::MultiByteToWide(volumeName));

        // Inspecting the volume reports the failure via an "Error" entry in its status.
        {
            wil::unique_cotaskmem_ansistring inspectOutput;
            VERIFY_SUCCEEDED(m_defaultSession->InspectVolume(volumeName.c_str(), &inspectOutput));
            auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectVolume>(inspectOutput.get());
            VERIFY_IS_TRUE(inspect.Status.has_value());
            VERIFY_IS_TRUE(inspect.Status->contains("Error"));

            // The backing .vhdx was deleted, so recovery fails to attach it with ERROR_FILE_NOT_FOUND.
            const auto expectedError = wsl::shared::string::WideToMultiByte(GetErrorString(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)));
            VERIFY_ARE_EQUAL(inspect.Status->at("Error"), expectedError);
        }

        // The unavailable volume can still be deleted once the container referencing it is removed.
        VERIFY_SUCCEEDED(recoveredContainer->Delete(WSLCDeleteFlagsForce));
        VERIFY_SUCCEEDED(m_defaultSession->DeleteVolume(volumeName.c_str()));
    }

    WSLC_TEST_METHOD(NamedVolumeGuestDriverOptsTest)
    {
        const std::string volumeName = "wslc-test-vol";
        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        auto expectReject = [&](const WSLCDriverOption* opts, ULONG optsCount, const std::wstring& expectedMessage) {
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

            WSLCVolumeOptions volumeOptions{};
            volumeOptions.Name = volumeName.c_str();
            volumeOptions.Driver = "guest";
            volumeOptions.DriverOpts = opts;
            volumeOptions.DriverOptsCount = optsCount;

            WSLCVolumeInformation volInfo{};
            VERIFY_ARE_EQUAL(m_defaultSession->CreateVolume(&volumeOptions, &volInfo), E_INVALIDARG);
            ValidateCOMErrorMessageContains(expectedMessage);
        };

        auto expectAccept = [&](const WSLCDriverOption* opts, ULONG optsCount) {
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

            WSLCVolumeOptions volumeOptions{};
            volumeOptions.Name = volumeName.c_str();
            volumeOptions.Driver = "guest";
            volumeOptions.DriverOpts = opts;
            volumeOptions.DriverOptsCount = optsCount;

            WSLCVolumeInformation volInfo{};
            VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));
        };

        // Allowed: no options (nullptr).
        expectAccept(nullptr, 0);

        // Allowed: type=tmpfs with device=tmpfs.
        {
            WSLCDriverOption opts[] = {{"type", "tmpfs"}, {"device", "tmpfs"}};
            expectAccept(opts, ARRAYSIZE(opts));
        }

        // Allowed: type=tmpfs with device=tmpfs and o= suboptions.
        {
            WSLCDriverOption opts[] = {{"type", "tmpfs"}, {"device", "tmpfs"}, {"o", "size=100m,uid=1000"}};
            expectAccept(opts, ARRAYSIZE(opts));
        }

        // Blocked: type=none (bind mount).
        {
            WSLCDriverOption opts[] = {{"type", "none"}};
            expectReject(opts, ARRAYSIZE(opts), L"unsupported volume driver options: type=none");
        }

        // Blocked: type=nfs.
        {
            WSLCDriverOption opts[] = {{"type", "nfs"}};
            expectReject(opts, ARRAYSIZE(opts), L"unsupported volume driver options: type=nfs");
        }

        // Blocked by Docker: device without type.
        {
            WSLCDriverOption opts[] = {{"device", "/some/path"}};
            expectReject(opts, ARRAYSIZE(opts), L"create wslc-test-vol: missing required option: \"type\"");
        }

        // Blocked by Docker: device=tmpfs without type.
        {
            WSLCDriverOption opts[] = {{"device", "tmpfs"}};
            expectReject(opts, ARRAYSIZE(opts), L"create wslc-test-vol: missing required option: \"type\"");
        }

        // Blocked by Docker: device and o without type.
        {
            WSLCDriverOption opts[] = {{"device", "tmpfs"}, {"o", "size=100m"}};
            expectReject(opts, ARRAYSIZE(opts), L"create wslc-test-vol: missing required option: \"type\"");
        }
    }

    WSLC_TEST_METHOD(NamedVolumeVhdOptionsParseTest)
    {
        const std::string volumeName = "wslc-volume-name";

        auto validateInvalidOptionsFailure = [&](const WSLCDriverOption* opts,
                                                 ULONG optsCount,
                                                 HRESULT expectedResult,
                                                 const std::optional<std::wstring>& expectedMessage = std::nullopt) {
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

            auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

            WSLCVolumeOptions volumeOptions{};
            volumeOptions.Name = volumeName.c_str();
            volumeOptions.Driver = "vhd";
            volumeOptions.DriverOpts = opts;
            volumeOptions.DriverOptsCount = optsCount;

            WSLCVolumeInformation volInfo{};
            const auto result = m_defaultSession->CreateVolume(&volumeOptions, &volInfo);

            if (result != expectedResult)
            {
                LogInfo("CreateVolume mismatch result=0x%08x expected=0x%08x", static_cast<unsigned int>(result), static_cast<unsigned int>(expectedResult));
            }

            VERIFY_ARE_EQUAL(result, expectedResult);
            if (expectedMessage.has_value())
            {
                ValidateCOMErrorMessage(expectedMessage);
            }
        };

        // Missing SizeBytes.
        validateInvalidOptionsFailure(nullptr, 0, E_INVALIDARG, L"Missing required option: 'SizeBytes'");

        WSLCDriverOption wrongOption[] = {{"WrongOption", "value"}};
        validateInvalidOptionsFailure(wrongOption, ARRAYSIZE(wrongOption), E_INVALIDARG, L"Missing required option: 'SizeBytes'");

        // Invalid SizeBytes values.
        WSLCDriverOption emptySize[] = {{"SizeBytes", ""}};
        validateInvalidOptionsFailure(emptySize, ARRAYSIZE(emptySize), E_INVALIDARG, L"Invalid value for option 'SizeBytes': ''");

        WSLCDriverOption zeroSize[] = {{"SizeBytes", "0"}};
        validateInvalidOptionsFailure(zeroSize, ARRAYSIZE(zeroSize), E_INVALIDARG, L"Invalid value for option 'SizeBytes': '0'");

        WSLCDriverOption invalidSizeAbc[] = {{"SizeBytes", "abc"}};
        validateInvalidOptionsFailure(
            invalidSizeAbc, ARRAYSIZE(invalidSizeAbc), E_INVALIDARG, L"Invalid value for option 'SizeBytes': 'abc'");

        WSLCDriverOption invalidSizeMixed[] = {{"SizeBytes", "123abc"}};
        validateInvalidOptionsFailure(
            invalidSizeMixed, ARRAYSIZE(invalidSizeMixed), E_INVALIDARG, L"Invalid value for option 'SizeBytes': '123abc'");

        WSLCDriverOption invalidSizeSign[] = {{"SizeBytes", "+-1"}};
        validateInvalidOptionsFailure(
            invalidSizeSign, ARRAYSIZE(invalidSizeSign), E_INVALIDARG, L"Invalid value for option 'SizeBytes': '+-1'");

        WSLCDriverOption invalidSizeOverflow[] = {{"SizeBytes", "18446744073709551616"}};
        validateInvalidOptionsFailure(
            invalidSizeOverflow,
            ARRAYSIZE(invalidSizeOverflow),
            E_INVALIDARG,
            L"Invalid value for option 'SizeBytes': '18446744073709551616'");

        WSLCDriverOption invalidSizeNeg[] = {{"SizeBytes", "-1"}};
        validateInvalidOptionsFailure(
            invalidSizeNeg, ARRAYSIZE(invalidSizeNeg), E_INVALIDARG, L"Invalid value for option 'SizeBytes': '-1'");

        // Invalid Fixed values.
        WSLCDriverOption invalidFixed[] = {{"SizeBytes", "1073741824"}, {"Fixed", "yes"}};
        validateInvalidOptionsFailure(
            invalidFixed, ARRAYSIZE(invalidFixed), E_INVALIDARG, L"Invalid value for option 'Fixed': 'yes'");

        WSLCDriverOption emptyFixed[] = {{"SizeBytes", "1073741824"}, {"Fixed", ""}};
        validateInvalidOptionsFailure(emptyFixed, ARRAYSIZE(emptyFixed), E_INVALIDARG, L"Invalid value for option 'Fixed': ''");

        // Invalid Uid values. Tests pair Uid with a valid Gid because Parse
        // requires both to be present together.
        WSLCDriverOption negUid[] = {{"SizeBytes", "1073741824"}, {"Uid", "-1"}, {"Gid", "0"}};
        validateInvalidOptionsFailure(negUid, ARRAYSIZE(negUid), E_INVALIDARG, L"Invalid value for option 'Uid': '-1'");

        WSLCDriverOption abcUid[] = {{"SizeBytes", "1073741824"}, {"Uid", "abc"}, {"Gid", "0"}};
        validateInvalidOptionsFailure(abcUid, ARRAYSIZE(abcUid), E_INVALIDARG, L"Invalid value for option 'Uid': 'abc'");

        WSLCDriverOption hugeUid[] = {{"SizeBytes", "1073741824"}, {"Uid", "4294967296"}, {"Gid", "0"}}; // 2^32, exceeds uint32_t max
        validateInvalidOptionsFailure(hugeUid, ARRAYSIZE(hugeUid), E_INVALIDARG, L"Invalid value for option 'Uid': '4294967296'");

        // Invalid Gid values.
        WSLCDriverOption negGid[] = {{"SizeBytes", "1073741824"}, {"Uid", "0"}, {"Gid", "-1"}};
        validateInvalidOptionsFailure(negGid, ARRAYSIZE(negGid), E_INVALIDARG, L"Invalid value for option 'Gid': '-1'");

        // Uid without Gid (or vice versa) is rejected.
        WSLCDriverOption uidOnly[] = {{"SizeBytes", "1073741824"}, {"Uid", "1000"}};
        validateInvalidOptionsFailure(uidOnly, ARRAYSIZE(uidOnly), E_INVALIDARG, L"Missing required option: 'Gid'");

        WSLCDriverOption gidOnly[] = {{"SizeBytes", "1073741824"}, {"Gid", "1000"}};
        validateInvalidOptionsFailure(gidOnly, ARRAYSIZE(gidOnly), E_INVALIDARG, L"Missing required option: 'Uid'");

        // Unknown options are rejected (catches typos and unsupported keys).
        WSLCDriverOption unknownOpt[] = {{"SizeBytes", "1073741824"}, {"Bogus", "value"}};
        validateInvalidOptionsFailure(unknownOpt, ARRAYSIZE(unknownOpt), E_INVALIDARG, L"Unknown option: 'Bogus'");
    }

    WSLC_TEST_METHOD(NamedVolumesVhdOwnership)
    {
        // Verify Uid/Gid are baked into the root inode at mkfs time so a
        // non-root container user can write to the volume.
        const std::string volumeName = "wslc-test-vhd-ownership";

        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        // nobody/nogroup are typically uid=65534 / gid=65534 on Debian.
        WSLCDriverOption driverOpts[] = {{"SizeBytes", "1073741824"}, {"Uid", "65534"}, {"Gid", "65534"}};

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = "vhd";
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));

        // A container running as 'nobody' should be able to write to the volume.
        {
            WSLCContainerLauncher writer(
                "debian:latest", "vhd-ownership-writer", {"/bin/sh", "-c", "echo non-root >/data/marker.txt"});
            writer.AddNamedVolume(volumeName, "/data", false);
            writer.SetUser("nobody:nogroup");

            auto writerContainer = writer.Launch(*m_defaultSession);
            auto writerProcess = writerContainer.GetInitProcess();
            ValidateProcessOutput(writerProcess, {});
        }

        // Verify the file is owned by the same uid/gid as the volume root.
        {
            WSLCContainerLauncher checker(
                "debian:latest", "vhd-ownership-checker", {"/bin/sh", "-c", "stat -c '%u %g' /data && cat /data/marker.txt"});
            checker.AddNamedVolume(volumeName, "/data", true);

            auto checkerContainer = checker.Launch(*m_defaultSession);
            auto checkerProcess = checkerContainer.GetInitProcess();
            ValidateProcessOutput(checkerProcess, {{1, "65534 65534\nnon-root\n"}});
        }
    }

    WSLC_TEST_METHOD(NamedVolumesVhdFixed)
    {
        // Fixed=true produces a .vhdx whose on-disk size is at least SizeBytes.
        const std::string volumeName = "wslc-test-vhd-fixed";
        const std::filesystem::path volumeVhdPath = m_storagePath / "volumes" / (volumeName + ".vhdx");
        constexpr ULONGLONG c_sizeBytes = 64 * _1MB;

        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        const auto sizeBytesStr = std::to_string(c_sizeBytes);
        WSLCDriverOption driverOpts[] = {{"SizeBytes", sizeBytesStr.c_str()}, {"Fixed", "true"}};

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = "vhd";
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));

        VERIFY_IS_TRUE(std::filesystem::exists(volumeVhdPath));
        const auto fileSize = std::filesystem::file_size(volumeVhdPath);

        // A dynamic VHD for a 64MB volume is typically a few MB; a fixed VHD
        // pre-allocates the full payload (>= SizeBytes).
        VERIFY_IS_GREATER_THAN_OR_EQUAL(fileSize, c_sizeBytes);
    }

    WSLC_TEST_METHOD(ListAndInspectNamedVolumesTest)
    {
        const std::string vhdVolumeName = "wsla-test-vol-vhd";
        const std::string guestVolumeName = "wsla-test-vol-guest";

        auto cleanup = wil::scope_exit([&]() {
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(vhdVolumeName.c_str()));
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(guestVolumeName.c_str()));
        });

        // Verify empty list is returned when no volumes exist.
        VERIFY_IS_TRUE(ListVolumes().empty());

        // Create a VHD volume and verify list returns one entry.
        WSLCDriverOption driverOpts[] = {{"SizeBytes", "1073741824"}};

        WSLCVolumeOptions vhdOptions{};
        vhdOptions.Name = vhdVolumeName.c_str();
        vhdOptions.Driver = "vhd";
        vhdOptions.DriverOpts = driverOpts;
        vhdOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&vhdOptions, &volInfo));

        auto volumes = ListVolumeEntries();
        VERIFY_ARE_EQUAL(1u, volumes.size());
        VERIFY_ARE_EQUAL(volumes[0].Name, vhdVolumeName);
        VERIFY_ARE_EQUAL(volumes[0].Driver, std::string("vhd"));
        VERIFY_IS_FALSE(volumes[0].Mountpoint.empty());
        VERIFY_ARE_EQUAL(volumes[0].Scope, std::string("local"));

        // Verify that a guest volume cannot be created with the same name as an existing vhd volume.
        WSLCVolumeOptions duplicateGuestOptions{};
        duplicateGuestOptions.Name = vhdVolumeName.c_str();
        duplicateGuestOptions.Driver = "guest";
        VERIFY_ARE_EQUAL(m_defaultSession->CreateVolume(&duplicateGuestOptions, &volInfo), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

        // Create a guest volume and verify both drivers show up in the list.
        WSLCVolumeOptions guestOptions{};
        guestOptions.Name = guestVolumeName.c_str();
        guestOptions.Driver = "guest";
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&guestOptions, &volInfo));

        // Verify that a vhd volume cannot be created with the same name as an existing guest volume.
        WSLCVolumeOptions duplicateVhdOptions{};
        duplicateVhdOptions.Name = guestVolumeName.c_str();
        duplicateVhdOptions.Driver = "vhd";
        duplicateVhdOptions.DriverOpts = driverOpts;
        duplicateVhdOptions.DriverOptsCount = ARRAYSIZE(driverOpts);
        VERIFY_ARE_EQUAL(m_defaultSession->CreateVolume(&duplicateVhdOptions, &volInfo), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

        volumes = ListVolumeEntries();
        VERIFY_ARE_EQUAL(2u, volumes.size());

        std::map<std::string, std::string> namesToDrivers;
        for (const auto& v : volumes)
        {
            namesToDrivers.emplace(v.Name, v.Driver);
        }

        VERIFY_ARE_EQUAL(namesToDrivers[vhdVolumeName], std::string("vhd"));
        VERIFY_ARE_EQUAL(namesToDrivers[guestVolumeName], std::string("guest"));

        // Verify InspectVolume returns correct details for the VHD volume (driver opts present).
        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectVolume(vhdVolumeName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto vhdInspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectVolume>(output.get());
        VERIFY_ARE_EQUAL(vhdInspect.Name, vhdVolumeName);
        VERIFY_ARE_EQUAL(vhdInspect.Driver, std::string("vhd"));
        VERIFY_ARE_EQUAL(vhdInspect.Scope, std::string("local"));
        VERIFY_IS_FALSE(vhdInspect.Mountpoint.empty());
        VERIFY_IS_TRUE(vhdInspect.Options.has_value());
        VERIFY_IS_TRUE(vhdInspect.Options->contains("SizeBytes"));

        // Verify InspectVolume returns correct details for the guest volume (no driver opts).
        output.reset();
        VERIFY_SUCCEEDED(m_defaultSession->InspectVolume(guestVolumeName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto guestInspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectVolume>(output.get());
        VERIFY_ARE_EQUAL(guestInspect.Name, guestVolumeName);
        VERIFY_ARE_EQUAL(guestInspect.Driver, std::string("guest"));
        VERIFY_ARE_EQUAL(guestInspect.Scope, std::string("local"));
        VERIFY_IS_FALSE(guestInspect.Mountpoint.empty());
        VERIFY_IS_FALSE(guestInspect.Options.has_value());

        // Verify InspectVolume fails for a non-existent volume.
        output.reset();
        VERIFY_ARE_EQUAL(m_defaultSession->InspectVolume("does-not-exist", &output), WSLC_E_VOLUME_NOT_FOUND);

        // Delete the VHD volume and verify only the guest volume remains.
        VERIFY_SUCCEEDED(m_defaultSession->DeleteVolume(vhdVolumeName.c_str()));
        volumes = ListVolumeEntries();
        VERIFY_ARE_EQUAL(1u, volumes.size());
        VERIFY_ARE_EQUAL(volumes[0].Name, guestVolumeName);
        VERIFY_ARE_EQUAL(volumes[0].Driver, std::string("guest"));
    }

    WSLC_TEST_METHOD(ListVolumesFilters)
    {
        const std::string vhdA = "wslc-list-vhd-a";
        const std::string vhdB = "wslc-list-vhd-b";
        const std::string guestA = "wslc-list-guest-a";
        const std::string guestB = "wslc-list-guest-b";
        const std::string otherName = "wslc-list-other-name";
        const std::string emptyValVol = "wslc-list-empty-val";

        const std::vector<WSLCDriverOption> vhdOpts = {{"SizeBytes", "1073741824"}};

        auto cleanup = wil::scope_exit([&]() {
            for (const auto& name : {vhdA, vhdB, guestA, guestB, otherName, emptyValVol})
            {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(name.c_str()));
            }
        });

        CreateNamedVolume(vhdA, "vhd", {{"env", "prod"}}, vhdOpts);
        CreateNamedVolume(vhdB, "vhd", {{"env", "test"}, {"tier", "db"}}, vhdOpts);
        CreateNamedVolume(guestA, "guest", {{"env", "prod"}});
        CreateNamedVolume(guestB, "guest");
        CreateNamedVolume(otherName, "guest", {{"env", "test"}});
        CreateNamedVolume(emptyValVol, "guest", {{"marker", ""}});

        auto expectListFails = [&](HRESULT expected, const std::vector<WSLCFilter>& filters) {
            const WSLCFilter* filtersPtr = filters.empty() ? nullptr : filters.data();
            const ULONG filtersCount = static_cast<ULONG>(filters.size());

            wil::unique_cotaskmem_ansistring output;
            VERIFY_ARE_EQUAL(expected, m_defaultSession->ListVolumes(filtersPtr, filtersCount, &output));
        };

        auto expectList = [&](const std::vector<std::string>& expected,
                              const std::vector<WSLCFilter>& filters = {},
                              const std::source_location& source = std::source_location::current()) {
            std::vector<std::string> names;
            for (const auto& v : ListVolumeEntries(filters))
            {
                names.emplace_back(v.Name);
            }

            VerifyAreEqualUnordered(expected, names, source);
        };

        const std::vector<std::string> all{vhdA, vhdB, guestA, guestB, otherName, emptyValVol};

        // No filter returns every volume.
        expectList(all);

        // Filter by driver name.
        expectList({vhdA, vhdB}, {{"driver", "vhd"}});
        expectList({guestA, guestB, otherName, emptyValVol}, {{"driver", "guest"}});
        expectList({}, {{"driver", "nonexistent"}});

        // Filter by volume name.
        expectList({vhdA, vhdB}, {{"name", "vhd"}});

        // Anchored regex matches exactly one volume.
        const auto anchoredVhdA = "^" + vhdA + "$";
        expectList({vhdA}, {{"name", anchoredVhdA.c_str()}});

        // Regex name filter.
        expectList({vhdA, vhdB}, {{"name", "vhd-."}});

        // Filter by label key (any value matches): label=<key> form.
        expectList({vhdA, vhdB, guestA, otherName}, {{"label", "env"}});

        // Filter by label key=value.
        expectList({vhdA, guestA}, {{"label", "env=prod"}});

        // Multiple labels are AND'ed together.
        expectList({vhdB}, {{"label", "env=test"}, {"label", "tier=db"}});

        // Unknown label key matches nothing.
        expectList({}, {{"label", "nope"}});

        // Unknown name matches nothing.
        expectList({}, {{"name", "nope"}});

        // Combined driver + name + label filter.
        expectList({vhdA}, {{"driver", "vhd"}, {"name", "a"}, {"label", "env=prod"}});

        // Dangling filter is supported by docker. All our named test volumes
        // are unused, so they are all dangling; combine with a name prefix to
        // exclude any leftover dangling volumes from other tests.
        expectList(all, {{"dangling", "true"}, {"name", "^wslc-list-"}});

        // label=<key> (key-only) matches the volume with the marker label regardless of stored value.
        expectList({emptyValVol}, {{"label", "marker"}});

        // label=<key>= (explicit empty value) matches only volumes whose stored value is also the empty string.
        expectList({emptyValVol}, {{"label", "marker="}});

        // No volume stores `env` with an empty value, so env= matches nothing.
        expectList({}, {{"label", "env="}});

        // env (key-only) matches every volume that has the key, regardless of its stored value.
        expectList({vhdA, vhdB, guestA, otherName}, {{"label", "env"}});

        // Unknown filter keys are rejected.
        expectListFails(E_INVALIDARG, {{"bogus", "x"}});

        // Null filter key/value is rejected.
        expectListFails(E_POINTER, {{nullptr, "anything"}});
        expectListFails(E_POINTER, {{"label", nullptr}});
    }

    WSLC_TEST_METHOD(PruneVolumesTest)
    {
        auto expectPrune = [&](const std::vector<std::string>& expected,
                               const std::vector<WSLCFilter>& filters = {},
                               const std::source_location& source = std::source_location::current()) {
            const WSLCFilter* filtersPtr = filters.empty() ? nullptr : filters.data();
            const ULONG filtersCount = static_cast<ULONG>(filters.size());

            wil::unique_cotaskmem_array_ptr<WSLCVolumeName> deleted;
            ULONGLONG spaceReclaimed = 0;
            VERIFY_SUCCEEDED(m_defaultSession->PruneVolumes(
                filtersPtr, filtersCount, nullptr, deleted.addressof(), deleted.size_address<ULONG>(), &spaceReclaimed));

            std::vector<std::string> names;
            for (const auto& n : deleted)
            {
                names.emplace_back(n);
            }

            VerifyAreEqualUnordered(expected, names, source);
        };

        // Prune with no eligible volumes (none created yet) returns an empty set.
        expectPrune({}, {{"all", "true"}});

        // Default (no all=true) only prunes anonymous volumes; with none present, returns empty.
        expectPrune({});

        // all=true prunes unused named guest volumes.
        {
            const std::string a = "wslc-prune-guest-a";
            const std::string b = "wslc-prune-guest-b";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(a.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(b.c_str()));
            });

            CreateNamedVolume(a, "guest");
            CreateNamedVolume(b, "guest");

            expectPrune({a, b}, {{"all", "true"}});

            auto volumes = ListVolumes();
            VERIFY_IS_FALSE(volumes.contains(a));
            VERIFY_IS_FALSE(volumes.contains(b));
        }

        // In-use volume is not pruned.
        {
            const std::string name = "wslc-prune-in-use";
            CreateNamedVolume(name, "guest");

            auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(name.c_str())); });

            WSLCContainerLauncher launcher("debian:latest", "wslc-prune-in-use-holder", {"sleep", "99999"});
            launcher.AddNamedVolume(name, "/data", false);
            auto container = launcher.Launch(*m_defaultSession);

            expectPrune({}, {{"all", "true"}});
            VERIFY_IS_TRUE(ListVolumes().contains(name));

            VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGKILL));
        }

        // Label filter (present, key=value).
        {
            const std::string labeled = "wslc-prune-labeled";
            const std::string unlabeled = "wslc-prune-unlabeled";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(labeled.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(unlabeled.c_str()));
            });

            CreateNamedVolume(labeled, "guest", {{"wslc-prune-test", "yes"}});
            CreateNamedVolume(unlabeled, "guest");

            expectPrune({labeled}, {{"all", "true"}, {"label", "wslc-prune-test=yes"}});
        }

        // Label filter (present, key only).
        {
            const std::string labeled = "wslc-prune-keyonly";
            const std::string unlabeled = "wslc-prune-keyonly-no";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(labeled.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(unlabeled.c_str()));
            });

            CreateNamedVolume(labeled, "guest", {{"wslc-prune-keyonly", "anything"}});
            CreateNamedVolume(unlabeled, "guest");

            // Value without '=' matches any volume with the key (Docker `label=key`).
            expectPrune({labeled}, {{"all", "true"}, {"label", "wslc-prune-keyonly"}});
        }

        // Label filter (absent, key only).
        {
            const std::string keep = "wslc-prune-keep";
            const std::string drop = "wslc-prune-drop";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(keep.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(drop.c_str()));
            });

            CreateNamedVolume(keep, "guest", {{"wslc-prune-keep", "yes"}});
            CreateNamedVolume(drop, "guest");

            // `label!` filters out volumes that have the key (Docker `label!=key`).
            expectPrune({drop}, {{"all", "true"}, {"label!", "wslc-prune-keep"}});
        }

        // VHD volumes are not pruned (docker skips bind-mount volumes).
        {
            const std::string vhdName = "wslc-prune-vhd-skip";
            const std::string guestName = "wslc-prune-vhd-skip-guest";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(vhdName.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(guestName.c_str()));
            });

            CreateNamedVolume(vhdName, "vhd", {}, {{"SizeBytes", "1073741824"}});
            CreateNamedVolume(guestName, "guest");

            expectPrune({guestName}, {{"all", "true"}});

            VERIFY_IS_TRUE(ListVolumes().contains(vhdName));
        }

        // ListVolumes / InspectVolume reflect prune results.
        {
            const std::string name = "wslc-prune-listsync";
            CreateNamedVolume(name, "guest");

            expectPrune({name}, {{"all", "true"}});
            VERIFY_IS_FALSE(ListVolumes().contains(name));
        }

        // Filter with null Key rejected.
        {
            WSLCFilter filters[] = {{nullptr, "true"}};

            wil::unique_cotaskmem_array_ptr<WSLCVolumeName> deleted;
            ULONGLONG spaceReclaimed = 0;

            VERIFY_ARE_EQUAL(
                E_POINTER,
                m_defaultSession->PruneVolumes(
                    filters, ARRAYSIZE(filters), nullptr, deleted.addressof(), deleted.size_address<ULONG>(), &spaceReclaimed));
        }

        // Filter with null Value rejected.
        {
            WSLCFilter filters[] = {{"label", nullptr}};

            wil::unique_cotaskmem_array_ptr<WSLCVolumeName> deleted;
            ULONGLONG spaceReclaimed = 0;

            VERIFY_ARE_EQUAL(
                E_POINTER,
                m_defaultSession->PruneVolumes(
                    filters, ARRAYSIZE(filters), nullptr, deleted.addressof(), deleted.size_address<ULONG>(), &spaceReclaimed));
        }
    }
};
