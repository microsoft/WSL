// Copyright (c) Microsoft. All rights reserved.
//
// C# P/Invoke interop for the WSL Container SDK (wslcsdk.dll).
//
// This file is a direct mapping of wslcsdk.h. All structures, enums, delegates,
// and entry points are exposed for use by managed callers.
//
// Notes:
//   - Opaque settings structs (WslcSessionSettings, WslcContainerSettings,
//     WslcProcessSettings) must only be manipulated through the SDK functions.
//   - Handle types (WslcSession, WslcContainer, WslcProcess) are IntPtr values
//     that must be released with the corresponding WslcRelease* function.
//   - Output strings allocated by the SDK (errorMessage, inspectData) are
//     CoTaskMem-allocated; free them with Marshal.FreeCoTaskMem after use.
//   - Delegate instances passed as callbacks must be kept alive (prevent GC)
//     for the entire duration they may be invoked by native code.

using System;
using System.Runtime.InteropServices;

namespace Microsoft.WSL.Containers.Interop
{
    [StructLayout(LayoutKind.Sequential, Size = 80, Pack = 8)]
    public struct WslcSessionSettings
    {
        // Opaque internal data - do not access directly.
    }

    [StructLayout(LayoutKind.Sequential, Size = 96, Pack = 8)]
    public struct WslcContainerSettings
    {
        // Opaque internal data - do not access directly.
    }


    [StructLayout(LayoutKind.Sequential, Size = 72, Pack = 8)]
    public struct WslcProcessSettings
    {
        // Opaque internal data - do not access directly.
    }

    public enum WslcContainerNetworkingMode : int
    {
        None = 0,
        Bridged = 1
    }

    public enum WslcVhdType : int
    {
        Dynamic = 0,
        Fixed = 1
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcVhdRequirements
    {
        // Ignored by WslcSetSessionSettingsVhd
        [MarshalAs(UnmanagedType.LPStr)]
        public string Name;

        public ulong SizeInBytes;
        public WslcVhdType Type;
    }

    [Flags]
    public enum WslcSessionFeatureFlags : int
    {
        None = 0x00000000,
        EnableGpu = 0x00000004
    }

    public enum WslcSessionTerminationReason : int
    {
        Unknown = 0,
        Shutdown = 1,
        Crashed = 2
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate void WslcSessionTerminationCallback(
        WslcSessionTerminationReason reason,
        IntPtr context);

    public enum WslcPortProtocol : int
    {
        Tcp = 0,
        Udp = 1
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcContainerPortMapping
    {
        public ushort WindowsPort;
        public ushort ContainerPort;
        public WslcPortProtocol Protocol;
        /// <summary>
        /// Optional pointer to a SockAddrStorage containing an IPv4 or IPv6 binding address.
        /// Set to IntPtr.Zero to use the default (127.0.0.1).
        /// </summary>
        public IntPtr WindowsAddress;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcContainerVolume
    {
        [MarshalAs(UnmanagedType.LPWStr)]
        public string WindowsPath;

        [MarshalAs(UnmanagedType.LPStr)]
        public string ContainerPath;

        [MarshalAs(UnmanagedType.Bool)]
        public bool ReadOnly;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcContainerNamedVolume
    {
        [MarshalAs(UnmanagedType.LPStr)]
        public string Name;

        [MarshalAs(UnmanagedType.LPStr)]
        public string ContainerPath;

        [MarshalAs(UnmanagedType.Bool)]
        public bool ReadOnly;
    }

    [Flags]
    public enum WslcContainerFlags : int
    {
        None = 0x00000000,
        AutoRemove = 0x00000001,
        EnableGpu = 0x00000002,
        Privileged = 0x00000004
    }

    [Flags]
    public enum WslcContainerStartFlags : int
    {
        None = 0x00000000,
        Attach = 0x00000001
    }

    public enum WslcContainerState : int
    {
        Invalid = 0,
        Created = 1,
        Running = 2,
        Exited = 3,
        Deleted = 4
    }

    public enum WslcSignal : int
    {
        None = 0,
        SigHup = 1,
        SigInt = 2,
        SigQuit = 3,
        SigKill = 9,
        SigTerm = 15
    }

    [Flags]
    public enum WslcDeleteContainerFlags : int
    {
        None = 0x00000000,
        Force = 0x00000001
    }

    public enum WslcProcessIOHandle : int
    {
        StdIn = 0,
        StdOut = 1,
        StdErr = 2
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate void WslcStdIOCallback(
        WslcProcessIOHandle ioHandle,
        IntPtr data,
        uint dataSize,
        IntPtr context);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate void WslcProcessExitCallback(
        int exitCode,
        IntPtr context);

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcProcessCallbacks
    {
        public WslcStdIOCallback OnStdOut;
        public WslcStdIOCallback OnStdErr;
        public WslcProcessExitCallback OnExit;
    }

    public enum WslcProcessState : int
    {
        Unknown = 0,
        Running = 1,
        Exited = 2,
        Signalled = 3
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcImageProgressDetail
    {
        public ulong Current;
        public ulong Total;
    }

    public enum WslcImageProgressStatus : int
    {
        Unknown = 0,
        Pulling = 1,
        Waiting = 2,
        Downloading = 3,
        Verifying = 4,
        Extracting = 5,
        Complete = 6
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcImageProgressMessage
    {
        public IntPtr Id; // PCSTR - use Marshal.PtrToStringAnsi

        public WslcImageProgressStatus Status;

        public WslcImageProgressDetail Detail;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcRegistryAuthenticationInformation
    {
        // TBD
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate int WslcContainerImageProgressCallback(
        ref WslcImageProgressMessage progress,
        IntPtr context);

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcPullImageOptions
    {
        [MarshalAs(UnmanagedType.LPStr)]
        public string Uri;

        public WslcContainerImageProgressCallback ProgressCallback;
        public IntPtr ProgressCallbackContext;
        public IntPtr AuthInfo; // pointer to WslcRegistryAuthenticationInformation, or IntPtr.Zero
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcImportImageOptions
    {
        public WslcContainerImageProgressCallback ProgressCallback;
        public IntPtr ProgressCallbackContext;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcLoadImageOptions
    {
        public WslcContainerImageProgressCallback ProgressCallback;
        public IntPtr ProgressCallbackContext;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct WslcImageInfo
    {
        public const int ImageNameLength = 256;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = ImageNameLength)]
        public string Name;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        public byte[] Sha256;

        public ulong SizeBytes;
        public ulong CreatedTimestamp;
    }

    [Flags]
    public enum WslcComponentFlags : int
    {
        None = 0,
        VirtualMachinePlatform = 1,
        WslPackage = 2
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WslcVersion
    {
        public uint Major;
        public uint Minor;
        public uint Revision;
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate void WslcInstallCallback(
        WslcComponentFlags component,
        uint progress,
        uint total,
        IntPtr context);

    // ========================================================================
    // Socket address helpers (for WslcContainerPortMapping)
    // ========================================================================

    /// <summary>
    /// Mirrors the native SOCKADDR_STORAGE structure (128 bytes).
    /// Use <see cref="SockAddrHelpers"/> to create instances for IPv4/IPv6.
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Size = 128)]
    public struct SockAddrStorage
    {
        public short Family; // AF_INET (2) or AF_INET6 (23)
    }

    [StructLayout(LayoutKind.Sequential, Size = 16)]
    public struct SockAddrIn
    {
        public short Family;    // AF_INET = 2
        public ushort Port;     // network byte order
        public uint Address;    // IPv4 address in network byte order

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
        public byte[] Zero;
    }

    [StructLayout(LayoutKind.Sequential, Size = 28)]
    public struct SockAddrIn6
    {
        public short Family;    // AF_INET6 = 23
        public ushort Port;     // network byte order
        public uint FlowInfo;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
        public byte[] Address;  // 128-bit IPv6 address

        public uint ScopeId;
    }

    /// <summary>
    /// Helpers to create native socket address structures for use with port mappings.
    /// </summary>
    public static class SockAddrHelpers
    {
        public const short AF_INET = 2;
        public const short AF_INET6 = 23;

        /// <summary>
        /// Creates a SockAddrStorage from a .NET IPAddress and pins it, returning
        /// a GCHandle the caller must free and a pointer suitable for
        /// WslcContainerPortMapping.WindowsAddress.
        /// </summary>
        public static GCHandle CreatePinned(System.Net.IPAddress address, out IntPtr pointer)
        {
            if (address == null)
                throw new ArgumentNullException(nameof(address));

            byte[] raw;

            if (address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)
            {
                raw = new byte[128];
                raw[0] = (byte)(AF_INET & 0xFF);
                raw[1] = (byte)((AF_INET >> 8) & 0xFF);
                byte[] addrBytes = address.GetAddressBytes(); // 4 bytes
                Buffer.BlockCopy(addrBytes, 0, raw, 4, 4);
            }
            else if (address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetworkV6)
            {
                raw = new byte[128];
                raw[0] = (byte)(AF_INET6 & 0xFF);
                raw[1] = (byte)((AF_INET6 >> 8) & 0xFF);
                byte[] addrBytes = address.GetAddressBytes(); // 16 bytes
                Buffer.BlockCopy(addrBytes, 0, raw, 8, 16);
                long scopeId = address.ScopeId;
                raw[24] = (byte)(scopeId & 0xFF);
                raw[25] = (byte)((scopeId >> 8) & 0xFF);
                raw[26] = (byte)((scopeId >> 16) & 0xFF);
                raw[27] = (byte)((scopeId >> 24) & 0xFF);
            }
            else
            {
                throw new ArgumentException("Only IPv4 and IPv6 addresses are supported.", nameof(address));
            }

            var handle = GCHandle.Alloc(raw, GCHandleType.Pinned);
            pointer = handle.AddrOfPinnedObject();
            return handle;
        }
    }

    // ========================================================================
    // Native methods (P/Invoke)
    // ========================================================================

    public static class WslcNativeMethods
    {
        private const string DllName = "wslcsdk.dll";
        public const int ContainerIdBufferSize = 65;

        // ----------------------------------------------------------------
        // Session
        // ----------------------------------------------------------------

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)]
        public static extern int WslcInitSessionSettings(
            [MarshalAs(UnmanagedType.LPWStr)] string name,
            [MarshalAs(UnmanagedType.LPWStr)] string storagePath,
            out WslcSessionSettings sessionSettings);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcCreateSession(
            ref WslcSessionSettings sessionSettings,
            out IntPtr session,
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetSessionSettingsCpuCount(
            ref WslcSessionSettings sessionSettings,
            uint cpuCount);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetSessionSettingsMemory(
            ref WslcSessionSettings sessionSettings,
            uint memoryMb);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetSessionSettingsTimeout(
            ref WslcSessionSettings sessionSettings,
            uint timeoutMS);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetSessionSettingsVhd(
            ref WslcSessionSettings sessionSettings,
            ref WslcVhdRequirements vhdRequirements);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, EntryPoint = "WslcSetSessionSettingsVhd")]
        public static extern int WslcSetSessionSettingsVhd(
            ref WslcSessionSettings sessionSettings,
            IntPtr vhdRequirements);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetSessionSettingsFeatureFlags(
            ref WslcSessionSettings sessionSettings,
            WslcSessionFeatureFlags flags);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetSessionSettingsTerminationCallback(
            ref WslcSessionSettings sessionSettings,
            WslcSessionTerminationCallback terminationCallback,
            IntPtr terminationContext);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcTerminateSession(IntPtr session);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcReleaseSession(IntPtr session);

        // ----------------------------------------------------------------
        // Container
        // ----------------------------------------------------------------

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public static extern int WslcInitContainerSettings(
            [MarshalAs(UnmanagedType.LPStr)] string imageName,
            out WslcContainerSettings containerSettings);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcCreateContainer(
            IntPtr session,
            ref WslcContainerSettings containerSettings,
            out IntPtr container,
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcStartContainer(
            IntPtr container,
            WslcContainerStartFlags flags,
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public static extern int WslcSetContainerSettingsName(
            ref WslcContainerSettings containerSettings,
            [MarshalAs(UnmanagedType.LPStr)] string name);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetContainerSettingsInitProcess(
            ref WslcContainerSettings containerSettings,
            ref WslcProcessSettings initProcess);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetContainerSettingsNetworkingMode(
            ref WslcContainerSettings containerSettings,
            WslcContainerNetworkingMode networkingMode);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public static extern int WslcSetContainerSettingsHostName(
            ref WslcContainerSettings containerSettings,
            [MarshalAs(UnmanagedType.LPStr)] string hostName);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public static extern int WslcSetContainerSettingsDomainName(
            ref WslcContainerSettings containerSettings,
            [MarshalAs(UnmanagedType.LPStr)] string domainName);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetContainerSettingsFlags(
            ref WslcContainerSettings containerSettings,
            WslcContainerFlags flags);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetContainerSettingsPortMappings(
            ref WslcContainerSettings containerSettings,
            [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] WslcContainerPortMapping[] portMappings,
            uint portMappingCount);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetContainerSettingsVolumes(
            ref WslcContainerSettings containerSettings,
            [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] WslcContainerVolume[] volumes,
            uint volumeCount);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetContainerSettingsNamedVolumes(
            ref WslcContainerSettings containerSettings,
            [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] WslcContainerNamedVolume[] namedVolumes,
            uint namedVolumeCount);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcCreateContainerProcess(
            IntPtr container,
            ref WslcProcessSettings newProcessSettings,
            out IntPtr newProcess,
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcReleaseContainer(IntPtr container);

        // ----------------------------------------------------------------
        // Container management
        // ----------------------------------------------------------------

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public static extern int WslcGetContainerID(
            IntPtr container,
            [MarshalAs(UnmanagedType.LPArray, SizeConst = ContainerIdBufferSize)] byte[] containerId);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcGetContainerInitProcess(
            IntPtr container,
            out IntPtr initProcess);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcInspectContainer(
            IntPtr container,
            out IntPtr inspectData);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcGetContainerState(
            IntPtr container,
            out WslcContainerState state);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcStopContainer(
            IntPtr container,
            WslcSignal signal,
            uint timeoutSeconds,
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcDeleteContainer(
            IntPtr container,
            WslcDeleteContainerFlags flags,
            out IntPtr errorMessage);

        // ----------------------------------------------------------------
        // Process
        // ----------------------------------------------------------------

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcInitProcessSettings(
            out WslcProcessSettings processSettings);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public static extern int WslcSetProcessSettingsCurrentDirectory(
            ref WslcProcessSettings processSettings,
            [MarshalAs(UnmanagedType.LPStr)] string currentDirectory);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetProcessSettingsCmdLine(
            ref WslcProcessSettings processSettings,
            [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] argv,
            UIntPtr argc);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetProcessSettingsEnvVariables(
            ref WslcProcessSettings processSettings,
            [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPStr)] string[] keyValue,
            UIntPtr argc);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSetProcessSettingsCallbacks(
            ref WslcProcessSettings processSettings,
            ref WslcProcessCallbacks callbacks,
            IntPtr context);

        // ----------------------------------------------------------------
        // Process management
        // ----------------------------------------------------------------

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcGetProcessPid(
            IntPtr process,
            out uint pid);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcGetProcessExitEvent(
            IntPtr process,
            out IntPtr exitEvent);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcGetProcessState(
            IntPtr process,
            out WslcProcessState state);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcGetProcessExitCode(
            IntPtr process,
            out int exitCode);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcSignalProcess(
            IntPtr process,
            WslcSignal signal);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcGetProcessIOHandle(
            IntPtr process,
            WslcProcessIOHandle ioHandle,
            out IntPtr handle);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcReleaseProcess(IntPtr process);

        // ----------------------------------------------------------------
        // Image management
        // ----------------------------------------------------------------

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcPullSessionImage(
            IntPtr session,
            ref WslcPullImageOptions options,
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public static extern int WslcImportSessionImage(
            IntPtr session,
            [MarshalAs(UnmanagedType.LPStr)] string imageName,
            IntPtr imageContent,
            ulong imageContentLength,
            IntPtr options, // pointer to WslcImportImageOptions, or IntPtr.Zero
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcImportSessionImageFromFile(
            IntPtr session,
            [MarshalAs(UnmanagedType.LPStr)] string imageName,
            [MarshalAs(UnmanagedType.LPWStr)] string path,
            IntPtr options, // pointer to WslcImportImageOptions, or IntPtr.Zero
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcLoadSessionImage(
            IntPtr session,
            IntPtr imageContent,
            ulong imageContentLength,
            IntPtr options, // pointer to WslcLoadImageOptions, or IntPtr.Zero
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcLoadSessionImageFromFile(
            IntPtr session,
            [MarshalAs(UnmanagedType.LPWStr)] string path,
            IntPtr options, // pointer to WslcLoadImageOptions, or IntPtr.Zero
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public static extern int WslcDeleteSessionImage(
            IntPtr session,
            [MarshalAs(UnmanagedType.LPStr)] string nameOrId,
            out IntPtr errorMessage);

        /// <summary>
        /// Lists container images. The returned array is CoTaskMem-allocated.
        /// Use <see cref="MarshalImageList"/> to convert to a managed array,
        /// then free the native pointer with Marshal.FreeCoTaskMem.
        /// </summary>
        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcListSessionImages(
            IntPtr session,
            out IntPtr images,
            out uint count);

        // ----------------------------------------------------------------
        // Storage
        // ----------------------------------------------------------------

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcCreateSessionVhdVolume(
            IntPtr session,
            ref WslcVhdRequirements options,
            out IntPtr errorMessage);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public static extern int WslcDeleteSessionVhdVolume(
            IntPtr session,
            [MarshalAs(UnmanagedType.LPStr)] string name,
            out IntPtr errorMessage);

        // ----------------------------------------------------------------
        // Install
        // ----------------------------------------------------------------

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcCanRun(
            [MarshalAs(UnmanagedType.Bool)] out bool canRun,
            out WslcComponentFlags missingComponents);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcGetVersion(
            out WslcVersion version);

        [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
        public static extern int WslcInstallWithDependencies(
            WslcInstallCallback progressCallback,
            IntPtr context);

        // ----------------------------------------------------------------
        // Managed helpers
        // ----------------------------------------------------------------

        /// <summary>
        /// Reads the CoTaskMem-allocated error message string and frees the native memory.
        /// Returns null if the pointer is IntPtr.Zero.
        /// </summary>
        public static string ConsumeErrorMessage(IntPtr errorMessage)
        {
            if (errorMessage == IntPtr.Zero)
                return null;

            string result = Marshal.PtrToStringUni(errorMessage);
            Marshal.FreeCoTaskMem(errorMessage);
            return result;
        }

        /// <summary>
        /// Reads the CoTaskMem-allocated ANSI string and frees the native memory.
        /// Returns null if the pointer is IntPtr.Zero.
        /// </summary>
        public static string ConsumeAnsiString(IntPtr ansiString)
        {
            if (ansiString == IntPtr.Zero)
                return null;

            string result = Marshal.PtrToStringAnsi(ansiString);
            Marshal.FreeCoTaskMem(ansiString);
            return result;
        }

        /// <summary>
        /// Reads a container ID from the byte buffer returned by WslcGetContainerID.
        /// </summary>
        public static string ContainerIdToString(byte[] containerId)
        {
            if (containerId == null)
                throw new ArgumentNullException(nameof(containerId));

            int length = Array.IndexOf(containerId, (byte)0);
            if (length < 0)
                length = containerId.Length;

            return System.Text.Encoding.ASCII.GetString(containerId, 0, length);
        }

        /// <summary>
        /// Marshals a native CoTaskMem-allocated WslcImageInfo array to a managed array
        /// and frees the native memory.
        /// </summary>
        public static WslcImageInfo[] MarshalImageList(IntPtr images, uint count)
        {
            if (images == IntPtr.Zero || count == 0)
                return Array.Empty<WslcImageInfo>();

            var result = new WslcImageInfo[count];
            int structSize = Marshal.SizeOf<WslcImageInfo>();

            for (uint i = 0; i < count; i++)
            {
                IntPtr current = IntPtr.Add(images, (int)(i * structSize));
                result[i] = Marshal.PtrToStructure<WslcImageInfo>(current);
            }

            Marshal.FreeCoTaskMem(images);
            return result;
        }

        /// <summary>
        /// Throws a COMException if the HRESULT indicates failure.
        /// </summary>
        public static void ThrowIfFailed(int hr)
        {
            if (hr < 0)
                Marshal.ThrowExceptionForHR(hr);
        }
    }
}