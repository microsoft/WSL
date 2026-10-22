// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <wrl/implements.h>

namespace wsl::windows::common {

namespace details {

    class RuntimeClassConstructorGuard
    {
    protected:
        RuntimeClassConstructorGuard() noexcept = default;

        ~RuntimeClassConstructorGuard()
        {
            if (m_releaseModule)
            {
                // If a derived constructor threw an exception, manually decrement the module reference.
                // This must happen after Microsoft::WRL::RuntimeClass is destroyed.
                if (auto* module = Microsoft::WRL::GetModuleBase())
                {
                    module->DecrementObjectCount();
                }
            }
        }

        bool m_releaseModule = false;
    };

} // namespace details

// This class is used to make sure that module references are correctly released even if the object's constructor throws.
// This is required because WRL's Make() call doesn't catch constructor exceptions, and so doesn't call Release().
template <typename... Interfaces>
class RuntimeClass : private details::RuntimeClassConstructorGuard, public Microsoft::WRL::RuntimeClass<Interfaces...>
{
protected:
    RuntimeClass() noexcept = default;

    ~RuntimeClass() override
    {
        // A non-zero reference count in the destructor means that Release() didn't run.
        // If that's the case, manually release a module reference for this instance.
        // This check must happen before Microsoft::WRL::RuntimeClass is destroyed.
        m_releaseModule = this->GetRefCount() != 0;
    }
};

} // namespace wsl::windows::common
