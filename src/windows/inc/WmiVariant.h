// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

// cpp headers
#include <vector>
#include <string>

// os headers
#include <Windows.h>
#include <objbase.h>
#include <wil/stl.h>
#include <wil/com.h>
#include <wil/resource.h>

// WmiMakeVariant(const ) functions are specializations designed to help callers
// who want a way to construct a VARIANT that is safe for passing into WMI
// since WMI has limitations on what VARIANT types it accepts
namespace wsl::core {
inline bool IsVariantEmptyOrNull(_In_ const VARIANT* variant) noexcept
{
    return V_VT(variant) == VT_EMPTY || V_VT(variant) == VT_NULL;
}

inline wil::unique_variant WmiMakeVariant(const bool value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_BOOL;
    V_BOOL(localVariant.addressof()) = value ? TRUE : FALSE;
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ bool* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_BOOL);
    *value = V_BOOL(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const char value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_UI1;
    V_UI1(localVariant.addressof()) = value;
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ char* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_UI1);
    *value = V_UI1(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const unsigned char value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_UI1;
    V_UI1(localVariant.addressof()) = value;
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ unsigned char* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_UI1);
    *value = V_UI1(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const short value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I2;
    V_I2(localVariant.addressof()) = value;
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ short* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I2);
    *value = V_I2(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const unsigned short value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I2;
    V_I2(localVariant.addressof()) = static_cast<short>(value);
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ unsigned short* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I2);
    *value = V_I2(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const long value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I4;
    V_I4(localVariant.addressof()) = value;
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ long* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I4);
    *value = V_I4(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const unsigned long value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I4;
    V_I4(localVariant.addressof()) = static_cast<long>(value);
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ unsigned long* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I4);
    *value = V_I4(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const int value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I4;
    V_I4(localVariant.addressof()) = value;
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ int* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I4);
    *value = V_I4(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const unsigned int value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_I4;
    V_I4(localVariant.addressof()) = static_cast<long>(value);
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ unsigned int* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_I4);
    *value = V_I4(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const float value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_R4;
    V_R4(localVariant.addressof()) = value;
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ float* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_R4);
    *value = V_R4(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const double value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_R8;
    V_R8(localVariant.addressof()) = value;
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ double* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_R8);
    *value = V_R8(variant);
    return true;
}

inline wil::unique_variant WmiMakeVariant(SYSTEMTIME value)
{
    wil::unique_variant localVariant;
    DOUBLE time{};
    THROW_HR_IF(E_INVALIDARG, !::SystemTimeToVariantTime(&value, &time));
    V_VT(localVariant.addressof()) = VT_DATE;
    V_DATE(localVariant.addressof()) = time;
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ SYSTEMTIME* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_DATE);
    THROW_HR_IF(E_INVALIDARG, !::VariantTimeToSystemTime(V_DATE(variant), value));
    return true;
}

inline wil::unique_variant WmiMakeVariant(_In_ const BSTR value) // NOLINT(misc-misplaced-const)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_BSTR;
    V_BSTR(localVariant.addressof()) = SysAllocString(value);
    THROW_IF_NULL_ALLOC(V_BSTR(localVariant.addressof()));
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ BSTR* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_BSTR);
    *value = SysAllocString(V_BSTR(variant));
    THROW_IF_NULL_ALLOC(*value);
    return true;
}

inline wil::unique_variant WmiMakeVariant(_In_ PCWSTR value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_BSTR;
    V_BSTR(localVariant.addressof()) = SysAllocString(value);
    THROW_IF_NULL_ALLOC(V_BSTR(localVariant.addressof()));
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::wstring* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_BSTR);
    value->assign(V_BSTR(variant));
    return true;
}

// Even though VARIANTs support 64-bit integers, WMI passes them around as BSTRs
inline wil::unique_variant WmiMakeVariant(const unsigned long long value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_BSTR;
    V_BSTR(localVariant.addressof()) = SysAllocString(std::to_wstring(value).c_str());
    THROW_IF_NULL_ALLOC(V_BSTR(localVariant.addressof()));
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ unsigned long long* value)
{
    *value = 0;
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_BSTR);
    *value = _wcstoui64(V_BSTR(variant), nullptr, 10);
    return true;
}

// Even though VARIANTs support 64-bit integers, WMI passes them around as BSTRs
inline wil::unique_variant WmiMakeVariant(_In_ const long long value)
{
    wil::unique_variant localVariant;
    V_VT(localVariant.addressof()) = VT_BSTR;
    V_BSTR(localVariant.addressof()) = SysAllocString(std::to_wstring(value).c_str());
    THROW_IF_NULL_ALLOC(V_BSTR(localVariant.addressof()));
    return localVariant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Out_ long long* value)
{
    *value = 0;
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_BSTR);
    *value = _wcstoi64(V_BSTR(variant), nullptr, 10);
    return true;
}

template <typename T>
wil::unique_variant WmiMakeVariant(const wil::com_ptr<T>& value) noexcept
{
    wil::unique_variant variant;
    V_VT(&variant) = VT_UNKNOWN;
    V_UNKNOWN(variant.addressof()) = value.get();
    // Must deliberately AddRef the raw pointer assigned to punkVal in the variant
    V_UNKNOWN(variant.addressof())->AddRef();
    return variant;
}

template <typename T>
bool WmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ wil::com_ptr<T>* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != VT_UNKNOWN);
    THROW_IF_FAILED(V_UNKNOWN(variant)->QueryInterface(__uuidof(T), reinterpret_cast<void**>(value->put())));
    return true;
}

template <typename T>
bool WmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::vector<wil::com_ptr<T>>* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != (VT_UNKNOWN | VT_ARRAY));

    IUnknown** iUnknownArray;
    THROW_IF_FAILED(::SafeArrayAccessData(variant->parray, reinterpret_cast<void**>(&iUnknownArray)));
    const auto unaccessArray = wil::scope_exit([&]() noexcept { SafeArrayUnaccessData(variant->parray); });

    std::vector<wil::com_ptr<T>> tempData;
    for (auto loop = 0ul; loop < variant->parray->rgsabound[0].cElements; ++loop)
    {
        wil::com_ptr<T> tempPtr;
        THROW_IF_FAILED(iUnknownArray[loop]->QueryInterface(__uuidof(T), reinterpret_cast<void**>(tempPtr.put())));
        tempData.push_back(tempPtr);
    }
    value->swap(tempData);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const std::vector<std::wstring>& data)
{
    auto* const tempSafeArray = SafeArrayCreateVector(VT_BSTR, 0, static_cast<ULONG>(data.size()));
    THROW_IF_NULL_ALLOC(tempSafeArray);
    auto guardArray = wil::scope_exit([&]() noexcept { SafeArrayDestroy(tempSafeArray); });

    for (size_t loop = 0; loop < data.size(); ++loop)
    {
        // SafeArrayPutElement requires an array of indexes for each dimension of the array
        // - in this case, we have a 1-dimensional array, thus an array of 1 LONG - assigned to the loop variable
        long index[1]{static_cast<long>(loop)};

        auto* const bstr = SysAllocString(data[loop].c_str());
        THROW_IF_NULL_ALLOC(bstr);
        THROW_IF_FAILED(::SafeArrayPutElement(tempSafeArray, index, bstr));
    }

    wil::unique_variant variant;
    variant.parray = tempSafeArray;
    variant.vt = VT_BSTR | VT_ARRAY;

    // don't free the SAFEARRAY on success - its lifetime is transferred to variant
    guardArray.release();
    return variant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::vector<std::wstring>* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != (VT_BSTR | VT_ARRAY));

    BSTR* stringArray{};
    THROW_IF_FAILED(::SafeArrayAccessData(variant->parray, reinterpret_cast<void**>(&stringArray)));
    const auto unaccessArray = wil::scope_exit([&]() noexcept { SafeArrayUnaccessData(variant->parray); });

    std::vector<std::wstring> tempData;
    for (auto loop = 0ul; loop < variant->parray->rgsabound[0].cElements; ++loop)
    {
        tempData.emplace_back(stringArray[loop]);
    }
    value->swap(tempData);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const std::vector<uint32_t>& data)
{
    auto* const tempSafeArray = SafeArrayCreateVector(VT_UI4, 0, static_cast<ULONG>(data.size()));
    THROW_IF_NULL_ALLOC(tempSafeArray);
    auto guardArray = wil::scope_exit([&]() noexcept { SafeArrayDestroy(tempSafeArray); });

    for (size_t loop = 0; loop < data.size(); ++loop)
    {
        // SafeArrayPutElement requires an array of indexes for each dimension of the array
        // - in this case, we have a 1-dimensional array, thus an array of 1 LONG - assigned to the loop variable
        long index[1]{static_cast<long>(loop)};

        uint32_t value = data[loop];
        THROW_IF_FAILED(::SafeArrayPutElement(tempSafeArray, index, &value));
    }

    wil::unique_variant variant;
    variant.parray = tempSafeArray;
    variant.vt = VT_UI4 | VT_ARRAY;

    // don't free the SAFEARRAY on success - its lifetime is transferred to variant
    guardArray.release();
    return variant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::vector<uint32_t>* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != (VT_UI4 | VT_ARRAY));

    uint32_t* intArray{};
    THROW_IF_FAILED(::SafeArrayAccessData(variant->parray, reinterpret_cast<void**>(&intArray)));
    const auto unaccessArray = wil::scope_exit([&]() noexcept { SafeArrayUnaccessData(variant->parray); });

    std::vector<uint32_t> tempData;
    for (auto loop = 0ul; loop < variant->parray->rgsabound[0].cElements; ++loop)
    {
        tempData.push_back(intArray[loop]);
    }
    value->swap(tempData);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const std::vector<unsigned short>& data)
{
    // WMI marshaler complains type mismatch using VT_UI2 | VT_ARRAY, and VT_I4 | VT_ARRAY works fine.
    auto* const tempSafeArray = SafeArrayCreateVector(VT_I4, 0, static_cast<ULONG>(data.size()));
    THROW_IF_NULL_ALLOC(tempSafeArray);
    auto guardArray = wil::scope_exit([&]() noexcept { SafeArrayDestroy(tempSafeArray); });

    for (size_t loop = 0; loop < data.size(); ++loop)
    {
        // SafeArrayPutElement requires an array of indexes for each dimension of the array
        // - in this case, we have a 1-dimensional array, thus an array of 1 LONG - assigned to the loop variable
        long index[1]{static_cast<long>(loop)};

        // Expand unsigned short to long because the SAFEARRAY created assumes VT_I4 elements
        long value = data[loop];
        THROW_IF_FAILED(::SafeArrayPutElement(tempSafeArray, index, &value));
    }

    wil::unique_variant variant;
    variant.parray = tempSafeArray;
    variant.vt = VT_I4 | VT_ARRAY;

    // don't free the SAFEARRAY on success - its lifetime is transferred to variant
    guardArray.release();
    return variant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::vector<unsigned short>* value)
{
    // WMI marshaler complains type mismatch using VT_UI2 | VT_ARRAY, and VT_I4 | VT_ARRAY works fine.
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != (VT_I4 | VT_ARRAY));

    long* intArray{};
    THROW_IF_FAILED(::SafeArrayAccessData(variant->parray, reinterpret_cast<void**>(&intArray)));
    const auto unaccessArray = wil::scope_exit([&]() noexcept { SafeArrayUnaccessData(variant->parray); });

    std::vector<unsigned short> tempData;
    for (auto loop = 0ul; loop < variant->parray->rgsabound[0].cElements; ++loop)
    {
        THROW_HR_IF(E_INVALIDARG, intArray[loop] > MAXUINT16);
        tempData.push_back(static_cast<unsigned short>(intArray[loop]));
    }
    value->swap(tempData);
    return true;
}

inline wil::unique_variant WmiMakeVariant(const std::vector<unsigned char>& data)
{
    auto* const tempSafeArray = SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(data.size()));
    THROW_IF_NULL_ALLOC(tempSafeArray);
    auto guardArray = wil::scope_exit([&]() noexcept { SafeArrayDestroy(tempSafeArray); });

    for (size_t loop = 0; loop < data.size(); ++loop)
    {
        // SafeArrayPutElement requires an array of indexes for each dimension of the array
        // - in this case, we have a 1-dimensional array, thus an array of 1 LONG - assigned to the loop variable
        long index[1]{static_cast<long>(loop)};

        unsigned char value = data[loop];
        THROW_IF_FAILED(::SafeArrayPutElement(tempSafeArray, index, &value));
    }

    wil::unique_variant variant;
    variant.parray = tempSafeArray;
    variant.vt = VT_UI1 | VT_ARRAY;

    // don't free the SAFEARRAY on success - its lifetime is transferred to variant
    guardArray.release();
    return variant;
}

inline bool WmiReadFromVariant(_In_ const VARIANT* variant, _Inout_ std::vector<unsigned char>* value)
{
    if (IsVariantEmptyOrNull(variant))
    {
        return false;
    }
    THROW_HR_IF(E_INVALIDARG, V_VT(variant) != (VT_UI1 | VT_ARRAY));

    unsigned char* charArray{};
    THROW_IF_FAILED(::SafeArrayAccessData(variant->parray, reinterpret_cast<void**>(&charArray)));
    const auto unaccessArray = wil::scope_exit([&]() noexcept { SafeArrayUnaccessData(variant->parray); });

    std::vector<unsigned char> tempData;
    for (auto loop = 0ul; loop < variant->parray->rgsabound[0].cElements; ++loop)
    {
        tempData.push_back(charArray[loop]);
    }
    value->swap(tempData);
    return true;
}
} // namespace wsl::core