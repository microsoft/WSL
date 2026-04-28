// Copyright (C) Microsoft Corporation. All rights reserved.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using WinRT;

namespace Microsoft.WSL.Containers;

internal static class WinRTActivation
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int DllGetActivationFactoryFn(IntPtr classId, out IntPtr factory);

    private static DllGetActivationFactoryFn s_getDllFactory;

    // Overrides the WinRT activation to route activation of our types through our native DLL.
    [ModuleInitializer]
    internal static void Initialize()
    {
        // Get a pointer to the function in the DLL that creates activation factories.
        s_getDllFactory = Marshal.GetDelegateForFunctionPointer<DllGetActivationFactoryFn>(
            NativeLibrary.GetExport(
                NativeLibrary.Load("wslcsdk.dll", typeof(WinRTActivation).Assembly, DllImportSearchPath.AssemblyDirectory),
                "DllGetActivationFactory"));

        // Custom WinRT activation handler:
        // If it is one of our types, we resolve it with our native DLL. Otherwise, we defer to the previous handler if one exists.
        var previousHandler = ActivationFactory.ActivationHandler;
        ActivationFactory.ActivationHandler = (typeName, iid) =>
        {
            if (typeName.StartsWith("Microsoft.WSL.Containers.", StringComparison.Ordinal))
            {
                return GetActivationFactory(typeName, iid);
            }

            if (previousHandler != null)
            {
                return previousHandler(typeName, iid);
            }

            return IntPtr.Zero;
        };

    }

    private static IntPtr GetActivationFactory(string typeName, Guid iid)
    {
        // Convert the type name to HSTRING
        WindowsCreateString(typeName, (uint)typeName.Length, out var hstring);
        try
        {
            if (s_getDllFactory(hstring, out var factory) < 0)
            {
                return IntPtr.Zero;
            }

            if (iid == IID_IActivationFactory)
            {
                return factory;
            }

            try
            {
                if (Marshal.QueryInterface(factory, ref iid, out var queried) >= 0)
                {
                    return queried;
                }
                else
                {
                    return IntPtr.Zero;
                }
            }
            finally
            {
                Marshal.Release(factory);
            }
        }
        finally
        {
            WindowsDeleteString(hstring);
        }
    }

    private static readonly Guid IID_IActivationFactory = new(0x00000035, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

    [DllImport("combase.dll", CharSet = CharSet.Unicode)]
    private static extern int WindowsCreateString(string sourceString, uint length, out IntPtr hstring);

    [DllImport("combase.dll")]
    private static extern int WindowsDeleteString(IntPtr hstring);
}
