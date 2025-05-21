// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

// cpp headers
#include <algorithm>
#include <iterator>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>
// os headers
// os headers
#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <WbemIdl.h>
// wil headers
#include <wil/stl.h>
#include <wil/com.h>
#include <wil/resource.h>

#include "WmiVariant.h"

namespace wsl::core {
// Callers must instantiate a WmiService instance in order to use any of the Wmi* classes
// This class tracks the WMI initialization of the IWbemLocator and IWbemService interfaces
// which maintain a connection to the specified WMI Service through which WMI calls are made
class WmiService
{
public:
    // CoInitializeSecurity is not called by the Wmi* classes. This security
    //   policy should be defined by the code consuming these libraries, as these
    //   libraries cannot assume the security context to apply to the process.
    explicit WmiService(PCWSTR path)
    {
        m_wbemLocator = wil::CoCreateInstance<WbemLocator, IWbemLocator>();

        THROW_IF_FAILED(m_wbemLocator->ConnectServer(
            wil::make_bstr(path).get(), // Object path of WMI namespace
            nullptr,                    // User name. NULL = current user
            nullptr,                    // User password. NULL = current
            nullptr,                    // Locale. NULL indicates current
            0,                          // Security flags.
            nullptr,                    // Authority (e.g. Kerberos)
            nullptr,                    // Context object
            m_wbemServices.put()));     // receive pointer to IWbemServices proxy

        THROW_IF_FAILED(CoSetProxyBlanket(
            m_wbemServices.get(),        // Indicates the proxy to set
            RPC_C_AUTHN_WINNT,           // RPC_C_AUTHN_xxx
            RPC_C_AUTHZ_NONE,            // RPC_C_AUTHZ_xxx
            nullptr,                     // Server principal name
            RPC_C_AUTHN_LEVEL_CALL,      // RPC_C_AUTHN_LEVEL_xxx
            RPC_C_IMP_LEVEL_IMPERSONATE, // RPC_C_IMP_LEVEL_xxx
            nullptr,                     // client identity
            EOAC_NONE));                 // proxy capabilities
    }

    ~WmiService() = default;
    WmiService(const WmiService& service) noexcept = default;
    WmiService& operator=(const WmiService& service) noexcept = default;
    WmiService(WmiService&& rhs) noexcept = default;
    WmiService& operator=(WmiService&& rhs) noexcept = default;

    IWbemServices* operator->() noexcept
    {
        return m_wbemServices.get();
    }

    const IWbemServices* operator->() const noexcept
    {
        return m_wbemServices.get();
    }

    bool operator==(const WmiService& service) const noexcept
    {
        return m_wbemLocator == service.m_wbemLocator && m_wbemServices == service.m_wbemServices;
    }

    bool operator!=(const WmiService& service) const noexcept
    {
        return !(*this == service);
    }

    IWbemServices* get() noexcept
    {
        return m_wbemServices.get();
    }

    [[nodiscard]] const IWbemServices* get() const noexcept
    {
        return m_wbemServices.get();
    }

    void delete_path(PCWSTR objPath, const wil::com_ptr<IWbemContext>& context) const
    {
        wil::com_ptr<IWbemCallResult> result;
        THROW_IF_FAILED(m_wbemServices->DeleteInstance(
            wil::make_bstr(objPath).get(), WBEM_FLAG_RETURN_IMMEDIATELY, context.get(), result.addressof()));
        // wait for the call to complete
        HRESULT status;
        THROW_IF_FAILED(result->GetCallStatus(WBEM_INFINITE, &status));
        THROW_IF_FAILED(status);
    }

    // Deletes the WMI object based off the object path specified in the input
    // The object path takes the form of:
    //    MyClass.MyProperty1='33',MyProperty2='value'
    void delete_path(PCWSTR objPath) const
    {
        const wil::com_ptr<IWbemContext> nullcontext;
        delete_path(objPath, nullcontext.get());
    }

private:
    wil::com_ptr<IWbemLocator> m_wbemLocator{};
    wil::com_ptr<IWbemServices> m_wbemServices{};
};

class WmiClassObject
{
public:
    //
    // forward declare iterator classes
    //
    class property_iterator;
    class method_iterator;

    WmiClassObject(WmiService wbemServices, wil::com_ptr<IWbemClassObject> wbemClass) noexcept :
        m_wbemServices(std::move(wbemServices)), m_wbemClassObject(std::move(wbemClass))
    {
    }

    WmiClassObject(WmiService wbemServices, PCWSTR className) : m_wbemServices(std::move(wbemServices))
    {
        THROW_IF_FAILED(m_wbemServices->GetObject(wil::make_bstr(className).get(), 0, nullptr, m_wbemClassObject.put(), nullptr));
    }

    WmiClassObject(WmiService wbemServices, BSTR className) : m_wbemServices(std::move(wbemServices))
    {
        THROW_IF_FAILED(m_wbemServices->GetObjectW(className, 0, nullptr, m_wbemClassObject.put(), nullptr));
    }

    [[nodiscard]] wil::com_ptr<IWbemClassObject> get_class_object() const noexcept
    {
        return m_wbemClassObject;
    }

    [[nodiscard]] property_iterator property_begin(bool fNonSystemPropertiesOnly = true) const
    {
        return property_iterator(m_wbemClassObject, fNonSystemPropertiesOnly);
    }

    [[nodiscard]] static property_iterator property_end() noexcept
    {
        return {};
    }

    // A forward property_iterator class type to enable forward-traversing instances of the queried WMI provider
    class property_iterator
    {
    public:
        property_iterator() = default;

        property_iterator(wil::com_ptr<IWbemClassObject> classObj, bool fNonSystemPropertiesOnly) :
            m_wbemClassObj(std::move(classObj)), m_index(0)
        {
            THROW_IF_FAILED(m_wbemClassObj->BeginEnumeration(fNonSystemPropertiesOnly ? WBEM_FLAG_NONSYSTEM_ONLY : 0));
            increment();
        }

        ~property_iterator() noexcept = default;
        property_iterator(const property_iterator&) = default;
        property_iterator& operator=(const property_iterator&) = default;
        property_iterator(property_iterator&&) = default;
        property_iterator& operator=(property_iterator&&) = default;

        void swap(property_iterator& rhs) noexcept
        {
            using std::swap;
            swap(m_index, rhs.m_index);
            swap(m_wbemClassObj, rhs.m_wbemClassObj);
            swap(m_propertyName, rhs.m_propertyName);
            swap(m_propertyType, rhs.m_propertyType);
        }

        ////////////////////////////////////////////////////////////////////////////////
        ///
        /// accessors:
        /// - dereference operators to access the property name
        /// - explicit type() method to expose its CIM type
        ///
        ////////////////////////////////////////////////////////////////////////////////
        BSTR operator*()
        {
            if (m_index == c_endIteratorIndex)
            {
                throw std::out_of_range("WmiClassObject::property_iterator::operator * - invalid subscript");
            }
            return m_propertyName.get();
        }

        BSTR operator*() const
        {
            if (m_index == c_endIteratorIndex)
            {
                throw std::out_of_range("WmiClassObject::property_iterator::operator * - invalid subscript");
            }
            return m_propertyName.get();
        }

        BSTR* operator->()
        {
            if (m_index == c_endIteratorIndex)
            {
                throw std::out_of_range("WmiClassObject::property_iterator::operator-> - invalid subscript");
            }
            return m_propertyName.addressof();
        }

        [[nodiscard]] CIMTYPE type() const
        {
            if (m_index == c_endIteratorIndex)
            {
                throw std::out_of_range("WmiClassObject::property_iterator::type - invalid subscript");
            }
            return m_propertyType;
        }

        bool operator==(const property_iterator& iter) const noexcept
        {
            if (m_index != c_endIteratorIndex)
            {
                return m_index == iter.m_index && m_wbemClassObj == iter.m_wbemClassObj;
            }
            return m_index == iter.m_index;
        }

        bool operator!=(const property_iterator& iter) const noexcept
        {
            return !(*this == iter);
        }

        // preincrement
        property_iterator& operator++()
        {
            increment();
            return *this;
        }

        // postincrement
        property_iterator operator++(int)
        {
            property_iterator temp(*this);
            increment();
            return temp;
        }

        // increment by integer
        property_iterator& operator+=(uint32_t inc)
        {
            for (auto loop = 0ul; loop < inc; ++loop)
            {
                increment();
                if (m_index == c_endIteratorIndex)
                {
                    throw std::out_of_range("WmiClassObject::property_iterator::operator+= - invalid subscript");
                }
            }
            return *this;
        }

        // property_iterator_traits
        // - allows <algorithm> functions to be used
        using iterator_category = std::forward_iterator_tag;
        using value_type = wil::shared_bstr;
        using difference_type = int;
        using pointer = BSTR;
        using reference = wil::shared_bstr&;

    private:
        static constexpr uint32_t c_endIteratorIndex = ULONG_MAX;

        wil::com_ptr<IWbemClassObject> m_wbemClassObj{};
        wil::shared_bstr m_propertyName;
        CIMTYPE m_propertyType = 0;
        uint32_t m_index = c_endIteratorIndex;

        void increment()
        {
            if (m_index == c_endIteratorIndex)
            {
                throw std::out_of_range("WmiClassObject::property_iterator - cannot increment: at the end");
            }

            CIMTYPE nextCimtype{};
            wil::shared_bstr nextName;
            switch (const auto hr = m_wbemClassObj->Next(0, nextName.put(), nullptr, &nextCimtype, nullptr))
            {
            case WBEM_S_NO_ERROR:
            {
                // update the instance members
                ++m_index;
                using std::swap;
                swap(m_propertyName, nextName);
                swap(m_propertyType, nextCimtype);
                break;
            }
            case WBEM_S_NO_MORE_DATA:
            {
                // at the end...
                m_index = c_endIteratorIndex;
                m_propertyName.reset();
                m_propertyType = 0;
                break;
            }

            default:
                THROW_HR(hr);
            }
        }
    };

private:
    WmiService m_wbemServices;
    wil::com_ptr<IWbemClassObject> m_wbemClassObject{};
};

class WmiInstance
{
public:
    // Constructors:
    // requires a IWbemServices object already connected to WMI
    // - one c'tor creates an empty instance (if set later)
    // - one c'tor takes the WMI class name to instantiate a new instance
    // - one c'tor takes an existing IWbemClassObject instance
    explicit WmiInstance(WmiService service) noexcept : m_wbemServices(std::move(service))
    {
    }

    WmiInstance(WmiService service, PCWSTR className) : m_wbemServices(std::move(service))
    {
        // get the object from the WMI service
        wil::com_ptr<IWbemClassObject> classObject;
        THROW_IF_FAILED(m_wbemServices->GetObject(wil::make_bstr(className).get(), 0, nullptr, classObject.addressof(), nullptr));
        // spawn an instance of this object
        THROW_IF_FAILED(classObject->SpawnInstance(0, m_instanceObject.put()));
    }

    WmiInstance(WmiService service, wil::com_ptr<IWbemClassObject> classObject) noexcept :
        m_wbemServices(std::move(service)), m_instanceObject(std::move(classObject))
    {
    }

    bool operator==(const WmiInstance& obj) const noexcept
    {
        return m_wbemServices == obj.m_wbemServices && m_instanceObject == obj.m_instanceObject;
    }

    bool operator!=(const WmiInstance& obj) const noexcept
    {
        return !(*this == obj);
    }

    [[nodiscard]] wil::com_ptr<IWbemClassObject> get_instance() const noexcept
    {
        return m_instanceObject;
    }

    [[nodiscard]] wil::unique_bstr get_path() const
    {
        wil::unique_variant objectPathVariant;
        get(L"__RELPATH", objectPathVariant.addressof());

        if (IsVariantEmptyOrNull(objectPathVariant.addressof()))
        {
            return nullptr;
        }

        if (V_VT(&objectPathVariant) != VT_BSTR)
        {
            THROW_HR(E_INVALIDARG);
        }

        return wil::make_bstr(V_BSTR(&objectPathVariant));
    }

    [[nodiscard]] WmiService get_service() const noexcept
    {
        return m_wbemServices;
    }

    // Retrieves the class name this WmiInstance is representing if any
    [[nodiscard]] wil::unique_bstr get_class_name() const
    {
        wil::unique_variant classVariant;
        get(L"__CLASS", classVariant.addressof());

        if (IsVariantEmptyOrNull(classVariant.addressof()))
        {
            return nullptr;
        }
        if (V_VT(&classVariant) != VT_BSTR)
        {
            THROW_HR(E_INVALIDARG);
        }

        return wil::make_bstr(V_BSTR(&classVariant));
    }

    // Returns a class object for the class represented by this instance
    [[nodiscard]] WmiClassObject get_class_object() const noexcept
    {
        return {m_wbemServices, m_instanceObject};
    }

    // Writes the instantiated object to the WMI repository
    // Supported wbemFlags:
    //   WBEM_FLAG_CREATE_OR_UPDATE
    //   WBEM_FLAG_UPDATE_ONLY
    //   WBEM_FLAG_CREATE_ONLY
    void write_instance(_In_opt_ IWbemContext* context, const LONG wbemFlags = WBEM_FLAG_CREATE_OR_UPDATE)
    {
        wil::com_ptr<IWbemCallResult> result;
        THROW_IF_FAILED(m_wbemServices->PutInstance(
            m_instanceObject.get(), wbemFlags | WBEM_FLAG_RETURN_IMMEDIATELY, context, result.addressof()));
        // wait for the call to complete
        HRESULT status;
        THROW_IF_FAILED(result->GetCallStatus(WBEM_INFINITE, &status));
        THROW_IF_FAILED(status);
    }

    void write_instance(LONG wbemFlags = WBEM_FLAG_CREATE_OR_UPDATE)
    {
        write_instance(nullptr, wbemFlags);
    }

    void delete_instance()
    {
        // delete the instance based off the __REPATH property
        wil::com_ptr<IWbemCallResult> result;
        THROW_IF_FAILED(m_wbemServices->DeleteInstance(get_path().get(), WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, result.addressof()));
        // wait for the call to complete
        HRESULT status;
        THROW_IF_FAILED(result->GetCallStatus(WBEM_INFINITE, &status));
        THROW_IF_FAILED(status);
    }

    // Invokes an instance method with zero -> 5 arguments from the instantiated IWbemClassObject
    // Returns a WmiInstance containing the [out] parameters from the method call
    // (the property "ReturnValue" contains the return value)
    WmiInstance execute_method(PCWSTR method)
    {
        return execute_method_impl(method, nullptr);
    }

    template <typename Arg1>
    WmiInstance execute_method(PCWSTR method, Arg1 arg1)
    {
        // establish the class object for the [in] params to the method
        wil::com_ptr<IWbemClassObject> inParamsDefinition;
        THROW_IF_FAILED(m_instanceObject->GetMethod(method, 0, inParamsDefinition.addressof(), nullptr));

        // spawn an instance to store the params
        wil::com_ptr<IWbemClassObject> inParamsInstance;
        THROW_IF_FAILED(inParamsDefinition->SpawnInstance(0, inParamsInstance.addressof()));

        // Instantiate a class object to iterate through each property
        const WmiClassObject propertyObject(m_wbemServices, inParamsDefinition);
        auto propertyIterator = propertyObject.property_begin();

        // write the property
        WmiInstance propertyclassObject(m_wbemServices, inParamsInstance);
        propertyclassObject.set(*propertyIterator, arg1);

        // execute the method with the properties set
        return execute_method_impl(method, inParamsInstance.get());
    }

    template <typename Arg1, typename Arg2>
    WmiInstance execute_method(PCWSTR method, Arg1 arg1, Arg2 arg2)
    {
        // establish the class object for the [in] params to the method
        wil::com_ptr<IWbemClassObject> inParamsDefinition;
        THROW_IF_FAILED(m_instanceObject->GetMethod(method, 0, inParamsDefinition.addressof(), nullptr));

        // spawn an instance to store the params
        wil::com_ptr<IWbemClassObject> inParamsInstance;
        THROW_IF_FAILED(inParamsDefinition->SpawnInstance(0, inParamsInstance.addressof()));

        // Instantiate a class object to iterate through each property
        const WmiClassObject propertyObject(m_wbemServices, inParamsDefinition);
        auto propertyIterator = propertyObject.property_begin();

        // write each property
        WmiInstance propertyclassObject(m_wbemServices, inParamsInstance);
        propertyclassObject.set(*propertyIterator, arg1);
        ++propertyIterator;
        propertyclassObject.set(*propertyIterator, arg2);

        // execute the method with the properties set
        return execute_method_impl(method, inParamsInstance.get());
    }

    template <typename Arg1, typename Arg2, typename Arg3>
    WmiInstance execute_method(PCWSTR method, Arg1 arg1, Arg2 arg2, Arg3 arg3)
    {
        // establish the class object for the [in] params to the method
        wil::com_ptr<IWbemClassObject> inParamsDefinition;
        THROW_IF_FAILED(m_instanceObject->GetMethod(method, 0, inParamsDefinition.addressof(), nullptr));

        // spawn an instance to store the params
        wil::com_ptr<IWbemClassObject> inParamsInstance;
        THROW_IF_FAILED(inParamsDefinition->SpawnInstance(0, inParamsInstance.addressof()));

        // Instantiate a class object to iterate through each property
        const WmiClassObject propertyObject(m_wbemServices, inParamsDefinition);
        auto propertyIterator = propertyObject.property_begin();

        // write each property
        WmiInstance propertyclassObject(m_wbemServices, inParamsInstance);
        propertyclassObject.set(*propertyIterator, arg1);
        ++propertyIterator;
        propertyclassObject.set(*propertyIterator, arg2);
        ++propertyIterator;
        propertyclassObject.set(*propertyIterator, arg3);

        // execute the method with the properties set
        return execute_method_impl(method, inParamsInstance.get());
    }

    template <typename Arg1, typename Arg2, typename Arg3, typename Arg4>
    WmiInstance execute_method(PCWSTR method, Arg1 arg1, Arg2 arg2, Arg3 arg3, Arg4 arg4)
    {
        // establish the class object for the [in] params to the method
        wil::com_ptr<IWbemClassObject> inParamsDefinition;
        THROW_IF_FAILED(m_instanceObject->GetMethod(method, 0, inParamsDefinition.addressof(), nullptr));

        // spawn an instance to store the params
        wil::com_ptr<IWbemClassObject> inParamsInstance;
        THROW_IF_FAILED(inParamsDefinition->SpawnInstance(0, inParamsInstance.addressof()));

        // Instantiate a class object to iterate through each property
        const WmiClassObject propertyObject(m_wbemServices, inParamsDefinition);
        auto propertyIterator = propertyObject.property_begin();

        // write each property
        WmiInstance propertyclassObject(m_wbemServices, inParamsInstance);
        propertyclassObject.set(*propertyIterator, arg1);
        ++propertyIterator;
        propertyclassObject.set(*propertyIterator, arg2);
        ++propertyIterator;
        propertyclassObject.set(*propertyIterator, arg3);
        ++propertyIterator;
        propertyclassObject.set(*propertyIterator, arg4);

        // execute the method with the properties set
        return execute_method_impl(method, inParamsInstance.get());
    }

    template <typename Arg1, typename Arg2, typename Arg3, typename Arg4, typename Arg5>
    WmiInstance execute_method(PCWSTR method, Arg1 arg1, Arg2 arg2, Arg3 arg3, Arg4 arg4, Arg5 arg5)
    {
        // establish the class object for the [in] params to the method
        wil::com_ptr<IWbemClassObject> inParamsDefinition;
        THROW_IF_FAILED(m_instanceObject->GetMethod(method, 0, inParamsDefinition.addressof(), nullptr));

        // spawn an instance to store the params
        wil::com_ptr<IWbemClassObject> inParamsInstance;
        THROW_IF_FAILED(inParamsDefinition->SpawnInstance(0, inParamsInstance.addressof()));

        // Instantiate a class object to iterate through each property
        const WmiClassObject propertyObject(m_wbemServices, inParamsDefinition);
        auto propertyIterator = propertyObject.property_begin();

        // write each property
        //
        WmiInstance propertyclassObject(m_wbemServices, inParamsInstance);
        propertyclassObject.set(*propertyIterator, arg1);
        ++propertyIterator;
        propertyclassObject.set(*propertyIterator, arg2);
        ++propertyIterator;
        propertyclassObject.set(*propertyIterator, arg3);
        ++propertyIterator;
        propertyclassObject.set(*propertyIterator, arg4);
        ++propertyIterator;
        propertyclassObject.set(*propertyIterator, arg5);

        // execute the method with the properties set
        return execute_method_impl(method, inParamsInstance.get());
    }

    bool is_null(PCWSTR propname) const
    {
        wil::unique_variant variant;
        get_property(propname, variant.addressof());
        return V_VT(&variant) == VT_NULL;
    }

    // get() and set()
    //
    //   Exposes the properties of the WMI object instantiated
    //   WMI instances don't use all VARIANT types - some specializations
    //   exist because, for example, 64-bit integers actually get passed through
    //   WMI as BSTRs (even though variants support 64-bit integers directly).
    //   See the MSDN documentation for WMI MOF Data Types (Numbers):
    //   http://msdn.microsoft.com/en-us/library/aa392716(v=VS.85).aspx
    //
    //   Even though VARIANTs support 16- and 32-bit unsigned integers, WMI passes them both
    //   around as 32-bit signed integers. Yes, that means you can't pass very large UINT32 values
    //   correctly through WMI directly.
    //
    // get() returns false if the value is empty or null
    //       returns true if retrieved the matching type

    bool get(PCWSTR propname, _Inout_ VARIANT* value) const
    {
        VariantClear(value);
        get_property(propname, value);
        return !IsVariantEmptyOrNull(value);
    }

    template <typename T>
    bool get(PCWSTR propname, _Out_ T* value) const
    {
        wil::unique_variant variant;
        get_property(propname, variant.addressof());
        return WmiReadFromVariant(variant.addressof(), value);
    }

    void set(PCWSTR propname, _In_ const VARIANT* value) const
    {
        set_property(propname, value);
    }

    template <typename T>
    void set(PCWSTR propname, const T value) const
    {
        set_property(propname, WmiMakeVariant(value).addressof());
    }

    // Calling IWbemClassObject::Delete on a property of an instance resets to the default value.
    void set_default(PCWSTR propname) const
    {
        THROW_IF_FAILED(m_instanceObject->Delete(propname));
    }

private:
    void get_property(PCWSTR propertyName, _Inout_ VARIANT* pVariant) const
    {
        auto* pInstance = m_instanceObject.get();
        THROW_IF_FAILED(pInstance->Get(propertyName, 0, pVariant, nullptr, nullptr));
    }

    void set_property(PCWSTR propname, _In_ const VARIANT* pVariant) const
    {
        THROW_IF_FAILED(m_instanceObject->Put(
            propname,
            0,
            const_cast<VARIANT*>(pVariant), // COM is not const-correct
            0));
    }

    WmiInstance execute_method_impl(PCWSTR method, _In_opt_ IWbemClassObject* pParams)
    {
        // exec the method semi-synchronously from this instance based off the __REPATH property
        wil::com_ptr<IWbemCallResult> result;
        THROW_IF_FAILED(m_wbemServices->ExecMethod(
            get_path().get(), wil::make_bstr(method).get(), WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, pParams, nullptr, result.addressof()));

        // wait for the call to complete - and get the [out] param object
        wil::com_ptr<IWbemClassObject> outParamsInstance;
        THROW_IF_FAILED(result->GetResultObject(WBEM_INFINITE, outParamsInstance.addressof()));

        // the call went through - return a WmiInstance from this retrieved instance
        return {m_wbemServices, outParamsInstance};
    }

    WmiService m_wbemServices;
    wil::com_ptr<IWbemClassObject> m_instanceObject{};
};

// Exposes enumerating instances of a WMI Provider through an iterator interface.
class WmiEnumerate
{
public:
    // A forward iterator class type to enable forward-traversing instances of the queried WMI provider
    //
    class iterator
    {
    public:
        explicit iterator(WmiService service) noexcept : m_wbemServices(std::move(service))
        {
        }

        iterator(WmiService service, wil::com_ptr<IEnumWbemClassObject> wbemEnumerator) :
            m_index(0), m_wbemServices(std::move(service)), m_wbemEnumerator(std::move(wbemEnumerator))
        {
            increment();
        }

        ~iterator() noexcept = default;
        iterator(const iterator&) noexcept = default;
        iterator& operator=(const iterator&) noexcept = default;
        iterator(iterator&&) noexcept = default;
        iterator& operator=(iterator&&) noexcept = default;

        void swap(_Inout_ iterator& rhs) noexcept
        {
            using std::swap;
            swap(m_index, rhs.m_index);
            swap(m_wbemServices, rhs.m_wbemServices);
            swap(m_wbemEnumerator, rhs.m_wbemEnumerator);
            swap(m_wmiInstance, rhs.m_wmiInstance);
        }

        [[nodiscard]] uint32_t location() const noexcept
        {
            return m_index;
        }

        WmiInstance& operator*() const noexcept
        {
            return *m_wmiInstance;
        }

        WmiInstance* operator->() const noexcept
        {
            return m_wmiInstance.get();
        }

        bool operator==(const iterator&) const noexcept;
        bool operator!=(const iterator&) const noexcept;

        iterator& operator++();         // preincrement
        iterator operator++(int);       // postincrement
        iterator& operator+=(uint32_t); // increment by integer

        // iterator_traits
        // - allows <algorithm> functions to be used
        using iterator_category = std::forward_iterator_tag;
        using value_type = WmiInstance;
        using difference_type = int;
        using pointer = WmiInstance*;
        using reference = WmiInstance&;

    private:
        void increment();

        static constexpr uint32_t c_endIteratorIndex = ULONG_MAX;
        uint32_t m_index = c_endIteratorIndex;
        WmiService m_wbemServices;
        wil::com_ptr<IEnumWbemClassObject> m_wbemEnumerator;
        std::shared_ptr<WmiInstance> m_wmiInstance;
    };

    explicit WmiEnumerate(WmiService wbemServices) noexcept : m_wbemServices(std::move(wbemServices))
    {
    }

    // Allows for executing a WMI query against the WMI service for an enumeration of WMI objects.
    // Assumes the query of of the WQL query language.
    const WmiEnumerate& query(_In_ PCWSTR query)
    {
        THROW_IF_FAILED(m_wbemServices->ExecQuery(
            wil::make_bstr(L"WQL").get(), wil::make_bstr(query).get(), WBEM_FLAG_BIDIRECTIONAL, nullptr, m_wbemEnumerator.put()));
        return *this;
    }

    const WmiEnumerate& query(_In_ PCWSTR query, const wil::com_ptr<IWbemContext>& context)
    {
        THROW_IF_FAILED(m_wbemServices->ExecQuery(
            wil::make_bstr(L"WQL").get(), wil::make_bstr(query).get(), WBEM_FLAG_BIDIRECTIONAL, context.get(), m_wbemEnumerator.put()));
        return *this;
    }

    iterator begin() const
    {
        if (nullptr == m_wbemEnumerator.get())
        {
            return end();
        }
        THROW_IF_FAILED(m_wbemEnumerator->Reset());
        return iterator(m_wbemServices, m_wbemEnumerator);
    }

    iterator end() const noexcept
    {
        return iterator(m_wbemServices);
    }

    iterator cbegin() const
    {
        if (nullptr == m_wbemEnumerator.get())
        {
            return cend();
        }
        THROW_IF_FAILED(m_wbemEnumerator->Reset());
        return iterator(m_wbemServices, m_wbemEnumerator);
    }

    iterator cend() const noexcept
    {
        return iterator(m_wbemServices);
    }

private:
    WmiService m_wbemServices;
    // Marking wbemEnumerator mutable to allow for const correctness of begin() and end()
    //   specifically, invoking Reset() is an implementation detail and should not affect external contracts
    mutable wil::com_ptr<IEnumWbemClassObject> m_wbemEnumerator;
};

inline bool WmiEnumerate::iterator::operator==(const iterator& iter) const noexcept
{
    if (m_index != c_endIteratorIndex)
    {
        return m_index == iter.m_index && m_wbemServices == iter.m_wbemServices && m_wbemEnumerator == iter.m_wbemEnumerator &&
               m_wmiInstance == iter.m_wmiInstance;
    }
    return m_index == iter.m_index && m_wbemServices == iter.m_wbemServices;
}

inline bool WmiEnumerate::iterator::operator!=(const iterator& iter) const noexcept
{
    return !(*this == iter);
}

// preincrement
inline WmiEnumerate::iterator& WmiEnumerate::iterator::operator++()
{
    increment();
    return *this;
}

// postincrement
inline WmiEnumerate::iterator WmiEnumerate::iterator::operator++(int)
{
    auto temp(*this);
    increment();
    return temp;
}

// increment by integer
inline WmiEnumerate::iterator& WmiEnumerate::iterator::operator+=(uint32_t inc)
{
    for (auto loop = 0ul; loop < inc; ++loop)
    {
        increment();
        if (m_index == c_endIteratorIndex)
        {
            throw std::out_of_range("WmiEnumerate::iterator::operator+= - invalid subscript");
        }
    }
    return *this;
}

inline void WmiEnumerate::iterator::increment()
{
    if (m_index == c_endIteratorIndex)
    {
        throw std::out_of_range("WmiEnumerate::iterator::increment at the end");
    }

    ULONG uReturn;
    wil::com_ptr<IWbemClassObject> wbemTarget;
    THROW_IF_FAILED(m_wbemEnumerator->Next(WBEM_INFINITE, 1, wbemTarget.put(), &uReturn));

    if (0 == uReturn)
    {
        // at the end...
        m_index = c_endIteratorIndex;
        m_wmiInstance.reset();
    }
    else
    {
        ++m_index;
        m_wmiInstance = std::make_shared<WmiInstance>(m_wbemServices, wbemTarget);
    }
}
} // namespace wsl::core