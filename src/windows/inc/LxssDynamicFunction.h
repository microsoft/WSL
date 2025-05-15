/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssDynamicFunction.h

Abstract:

    This file contains a helper for accessing dynamically loaded functions.

--*/

#pragma once

enum class DynamicFunctionErrorLogs
{
    None
};

/// <summary>
/// Wrapper for a runtime dynamically-loaded function.
/// </summary>
template <typename FunctionType>
class LxssDynamicFunction
{
public:
    /// <summary>
    /// Constructor.
    /// </summary>
    LxssDynamicFunction(const wil::shared_hmodule& module, LPCSTR functionName) :
        m_function{reinterpret_cast<FunctionType*>(GetProcAddress(module.get(), functionName))}, m_module{module}
    {
        THROW_LAST_ERROR_IF(!m_function);
    }

    /// <summary>
    /// Constructor that loads the module.
    /// </summary>
    LxssDynamicFunction(LPCWSTR moduleName, LPCSTR functionName) :
        LxssDynamicFunction{LoadLibraryHelper(moduleName), functionName}
    {
    }

    /// <summary>
    /// Constructor - this constructs an object that deliberately does not want exceptions (Telemetry Errors on failure)
    /// With this constructor, the caller must call load() later to attempt to load the specified module
    /// Note: there is not a default c'tor - we do not want accidental construction
    /// </summary>
    LxssDynamicFunction(DynamicFunctionErrorLogs) noexcept
    {
    }

    /// <summary>
    /// Attempt to dynamically load the function
    /// will return an error instead of throwing on failure - which can be needed when we do not want Error traces on failure
    /// </summary>
    HRESULT load(const wil::shared_hmodule& module, LPCSTR functionName) noexcept
    {
        m_module.reset();

        m_function = reinterpret_cast<FunctionType*>(GetProcAddress(module.get(), functionName));
        RETURN_LAST_ERROR_IF_EXPECTED(!m_function);
        m_module = module;
        return S_OK;
    }

    HRESULT load(LPCWSTR moduleName, LPCSTR functionName) noexcept
    {
        const wil::shared_hmodule module{LoadLibraryEx(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        RETURN_LAST_ERROR_IF_EXPECTED(!module);
        return load(module, functionName);
    }

    /// <summary>
    /// Call through to the dynamically loaded function.
    /// </summary>

    template <typename... Args>
    decltype(auto) operator()(Args&&... args)
    {
        return m_function(std::forward<Args>(args)...);
    }

private:
    static wil::shared_hmodule LoadLibraryHelper(LPCWSTR moduleName)
    {
        wil::shared_hmodule module{LoadLibraryEx(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        THROW_LAST_ERROR_IF_MSG(!module, "Failed to load %ls", moduleName);
        return module;
    }

    /// <summary>
    /// No default constructor.
    /// </summary>
    LxssDynamicFunction() = delete;

    /// <summary>
    /// Dynamically loaded function wrapper.
    /// </summary>
    FunctionType* m_function;

    /// <summary>
    /// Reference to the module containing the dynamically loaded function.
    /// </summary>
    wil::shared_hmodule m_module;
};
