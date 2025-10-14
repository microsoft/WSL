// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <optional>

#define P9_EXPECTED_STD std::

#include "result_macros.h"

namespace util {

// Represents an error condition with the specified type.
template <typename E>
struct Unexpected
{
    // Creates anew instance with the specified error value.
    // N.B. This constructor allows initialization without specifying the template argument.
    Unexpected(const E& value) : Value{value}
    {
    }

    E Value;
};

namespace details {

    template <typename T, typename... Args>
    void ConstructInPlace(T& value, Args&&... args)
    {
        new (P9_EXPECTED_STD addressof(value)) T(P9_EXPECTED_STD forward<Args>(args)...);
    }

    // Storage for the BasicExpected class for types that are trivially destructible.
    template <typename T, typename E>
    struct TrivialExpectedStorage
    {
        // Creates a new instance with a default initialized value.
        constexpr TrivialExpectedStorage() : HasValue{true}, Value{}
        {
        }

        // Creates a new instance by invoking a constructor on the value type.
        template <typename... Args>
        constexpr TrivialExpectedStorage(P9_EXPECTED_STD in_place_t, Args&&... args) :
            HasValue{true}, Value{P9_EXPECTED_STD forward<Args>(args)...}
        {
        }

        // Creates a new instance with the specified error.
        constexpr TrivialExpectedStorage(const Unexpected<E>& error) : HasValue{false}, Error{error.Value}
        {
        }

        // Move-constructs a new instance from an existing instance.
        constexpr TrivialExpectedStorage(TrivialExpectedStorage&& other) : HasValue{other.HasValue}, NoInit{}
        {
            if (other.HasValue)
            {
                ConstructInPlace(Value, P9_EXPECTED_STD move(other.Value));
            }
            else
            {
                ConstructInPlace(Error, P9_EXPECTED_STD move(other.Error));
            }
        }

        // Move-assigns the value of an existing instance.
        TrivialExpectedStorage& operator=(TrivialExpectedStorage&& other)
        {
            if (P9_EXPECTED_STD addressof(other) != this)
            {
                if (other.HasValue)
                {
                    if (HasValue)
                    {
                        Value = P9_EXPECTED_STD move(other.Value);
                    }
                    else
                    {
                        ConstructInPlace(Value, P9_EXPECTED_STD move(other.Value));
                    }
                }
                else
                {
                    if (HasValue)
                    {
                        ConstructInPlace(Error, P9_EXPECTED_STD move(other.Error));
                    }
                    else
                    {
                        Error = P9_EXPECTED_STD move(other.Error);
                    }
                }

                HasValue = other.HasValue;
            }

            return *this;
        }

        // Default destructor, because the value type is trivially destructible.
        ~TrivialExpectedStorage() = default;

        bool HasValue;

        union
        {
            T Value;
            E Error;
            char NoInit;
        };
    };

    // Storage for the BasicExpected class for types that are not trivially destructible.
    template <typename T, typename E>
    struct NonTrivialExpectedStorage
    {
        // Creates a new instance with a default initialized value.
        constexpr NonTrivialExpectedStorage() : HasValue{true}, Value{}
        {
        }

        // Creates a new instance by invoking a constructor on the value type.
        template <typename... Args>
        constexpr NonTrivialExpectedStorage(P9_EXPECTED_STD in_place_t, Args&&... args) :
            HasValue{true}, Value{P9_EXPECTED_STD forward<Args>(args)...}
        {
        }

        // Creates a new instance with the specified error.
        constexpr NonTrivialExpectedStorage(const Unexpected<E>& error) : HasValue{false}, Error{error.Value}
        {
        }

        // Move-constructs a new instance from an existing instance.
        constexpr NonTrivialExpectedStorage(NonTrivialExpectedStorage&& other) : HasValue{other.HasValue}, NoInit{}
        {
            if (other.HasValue)
            {
                ConstructInPlace(Value, P9_EXPECTED_STD move(other.Value));
            }
            else
            {
                ConstructInPlace(Error, P9_EXPECTED_STD move(other.Error));
            }
        }

        // Move-assigns the value of an existing instance.
        NonTrivialExpectedStorage& operator=(NonTrivialExpectedStorage&& other)
        {
            if (P9_EXPECTED_STD addressof(other) != this)
            {
                if (other.HasValue)
                {
                    if (HasValue)
                    {
                        Value = P9_EXPECTED_STD move(other.Value);
                    }
                    else
                    {
                        Error.~E();
                        ConstructInPlace(Value, P9_EXPECTED_STD move(other.Value));
                    }
                }
                else
                {
                    if (HasValue)
                    {
                        Value.~T();
                        ConstructInPlace(Error, P9_EXPECTED_STD move(other.Error));
                    }
                    else
                    {
                        Error = P9_EXPECTED_STD move(other.Error);
                    }
                }

                HasValue = other.HasValue;
            }

            return *this;
        }

        // Explicitly destructs either the value or the error.
        ~NonTrivialExpectedStorage()
        {
            if (HasValue)
            {
                Value.~T();
            }
            else
            {
                Error.~E();
            }
        }

        bool HasValue;

        union
        {
            T Value;
            E Error;
            char NoInit;
        };
    };

    // Class that helps to select between the storage for trivially and non-trivially destructible
    // types.
    template <typename T, typename E>
    struct ExpectedStorage
    {
        using Type = P9_EXPECTED_STD conditional_t<
            P9_EXPECTED_STD is_trivially_destructible<T>::value && P9_EXPECTED_STD is_trivially_destructible<E>::value,
            TrivialExpectedStorage<T, E>,
            NonTrivialExpectedStorage<T, E>>;
    };

} // namespace details

// This is a simple implementation of the proposed standard std::excepted type, which can be
// used as the return type for functions that return either a value or an error.
// N.B. This is named BasicExpected so that consumers can create an Expected typedef with their
//      most common error type.
template <typename T, typename E>
class BasicExpected
{
public:
    // Creates a new instance with a default initialized value.
    constexpr BasicExpected() = default;

    // Creates a new instance holding the specified value.
    constexpr BasicExpected(const T& value) : m_storage{P9_EXPECTED_STD in_place, value}
    {
    }

    // Creates a new instance moving the specified value.
    constexpr BasicExpected(T&& value) : m_storage{P9_EXPECTED_STD in_place, P9_EXPECTED_STD move(value)}
    {
    }

    // Creates a new instance holding the specified error.
    constexpr BasicExpected(const Unexpected<E>& error) : m_storage{error}
    {
    }

    // Creates a new instance moving the specified error.
    constexpr BasicExpected(Unexpected<E>&& error) : m_storage{P9_EXPECTED_STD move(error)}
    {
    }

    // Move-constructs a new instance from an existing instance.
    constexpr BasicExpected(BasicExpected&& other) : m_storage{P9_EXPECTED_STD move(other.m_storage)}
    {
    }

    // Move-assigns from an existing instance.
    BasicExpected& operator=(BasicExpected&& other)
    {
        if (P9_EXPECTED_STD addressof(other) != this)
        {
            m_storage = P9_EXPECTED_STD move(other.m_storage);
        }

        return *this;
    }

    // Tests whether or not the instance holds a value.
    explicit operator bool() const
    {
        return m_storage.HasValue;
    }

    // Gets a reference to the contained value.
    T& Get()
    {
        FAIL_FAST_IF(!m_storage.HasValue);
        return m_storage.Value;
    }

    // Gets a const-reference to the contained value.
    const T& Get() const
    {
        FAIL_FAST_IF(!m_storage.HasValue);
        return m_storage.Value;
    }

    // Accesses members of the contained value.
    // N.B. Behavior is undefined if the instance does not contain a value.
    T* operator->()
    {
        return P9_EXPECTED_STD addressof(m_storage.Value);
    }

    // Accesses members of the contained value.
    // N.B. Behavior is undefined if the instance does not contain a value.
    const T* operator->() const
    {
        return P9_EXPECTED_STD addressof(m_storage.Value);
    }

    // Gets a reference to the contained value.
    // N.B. Behavior is undefined if the instance does not contain a value.
    T& operator*()
    {
        return m_storage.Value;
    }

    // Gets a const-reference to the contained value.
    // N.B. Behavior is undefined if the instance does not contain a value.
    const T& operator*() const
    {
        return m_storage.Value;
    }

    // Gets the contained error value.
    // N.B. Behavior is undefined if the instance contains a value.
    const E& Error() const
    {
        return m_storage.Error;
    }

    // Gets the contained error value if the instance contains an error.
    // N.B. This function allows the RETURN_ macros to work without accessing their argument twice.
    // N.B. This creates a copy of the error value, but since the error value is usually expected
    //      to be a trivial type (e.g. int) that should not be a problem.
    P9_EXPECTED_STD optional<E> OptionalError() const
    {
        if (m_storage.HasValue)
        {
            return {};
        }

        return m_storage.Error;
    }

    // Gets the contained error value, wrapped in an Unexpected struct.
    // N.B. This makes it easy to return the error in a function that also returns a BasicExpected type.
    // N.B. Behavior is undefined if the instance contains a value.
    Unexpected<E> Unexpected() const
    {
        FAIL_FAST_IF(m_storage.HasValue);
        return m_storage.Error;
    }

private:
    typename details::ExpectedStorage<T, E>::Type m_storage;
};

} // namespace util
