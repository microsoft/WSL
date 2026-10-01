## Test Generation Guidelines for WSL

When generating tests for this repository, follow these patterns:

### Framework
Tests use TAEF (Test Authoring and Execution Framework). Always include `"Common.h"`.

### Test Class Structure
```cpp
#include "Common.h"

namespace MyFeatureTests
{
class MyFeatureTests
{
    WSL_TEST_CLASS(MyFeatureTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        LxsstuUninitialize(FALSE);
        return true;
    }

    TEST_METHOD(DescriptiveTestName)
    {
        // Test implementation
    }
};
}
```

### Key Rules
- Use `WSL_TEST_CLASS(Name)` — never raw `BEGIN_TEST_CLASS`
- Setup/cleanup methods must `return true` on success
- Use `VERIFY_*` macros for assertions — never `assert()` or exceptions for test validation

### Assertion Macros
- `VERIFY_ARE_EQUAL(expected, actual)` — value equality
- `VERIFY_ARE_NOT_EQUAL(a, b)` — value inequality
- `VERIFY_IS_TRUE(condition)` — boolean check
- `VERIFY_IS_FALSE(condition)` — negative boolean check
- `VERIFY_IS_NULL(ptr)` — null check
- `VERIFY_IS_NOT_NULL(ptr)` — non-null check
- `VERIFY_WIN32_BOOL_SUCCEEDED(expr)` — Win32 BOOL result
- `VERIFY_SUCCEEDED(hr)` — HRESULT success

### Logging in Tests
- `LogInfo(fmt, ...)` — informational messages
- `LogError(fmt, ...)` — error messages
- `LogWarning(fmt, ...)` — warnings
- `LogPass(fmt, ...)` — explicit pass messages
- `LogSkipped(fmt, ...)` — skip messages

### Conditional Skipping
Add skip macros at the start of a test method body when the test only applies to certain environments:
```cpp
TEST_METHOD(Wsl2SpecificTest)
{
    WSL2_TEST_ONLY();
    // ... test code ...
}
```

Available skip macros:
- `WSL1_TEST_ONLY()` — skip unless WSL1
- `WSL2_TEST_ONLY()` — skip unless WSL2
- `SKIP_TEST_ARM64()` — skip on ARM64
- `SKIP_TEST_UNSTABLE()` — skip known-flaky tests
- `WINDOWS_11_TEST_ONLY()` — skip on pre-Windows 11
- `WSL_TEST_VERSION_REQUIRED(version)` — skip if WSL version too old

### RAII Test Helpers
- `WslKeepAlive` — prevents UVM timeout during long-running tests; create at test start
- `WslConfigChange` — RAII wrapper that applies a temporary `.wslconfig` and restores the original on destruction:
```cpp
TEST_METHOD(TestWithCustomConfig)
{
    WslConfigChange config(L"[wsl2]\nmemory=4GB\n");
    // ... test with custom config ...
    // Original .wslconfig restored when config goes out of scope
}
```

### Memory in Tests
- Use `ALLOC(size)` / `FREE(ptr)` macros for direct heap allocation in tests
- Prefer RAII wrappers and smart pointers for production-like code paths

### Test Naming
- Use descriptive PascalCase names that describe the scenario: `CreateInstanceWithInvalidGuidFails`, `EchoTest`, `MountPlan9Share`
- Group related tests in the same test class
