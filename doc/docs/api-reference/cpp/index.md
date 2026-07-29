# WSL container API C++ reference

This reference documents the **C++/WinRT projection** in `Microsoft::WSL::Containers`.

> **Preview notice:** `wslcsdk.h` explicitly marks this API as **preview** and subject to breaking changes.
>
> **Header:** `#include <winrt/Microsoft.WSL.Containers.h>`
>
> **Namespace:** `winrt::Microsoft::WSL::Containers`

The projection is layered as **Session → Container → Process**. Errors surface as `winrt::hresult_error`. Image and installation operations use `IAsyncActionWithProgress<T>`.

---

## In this reference

- [Data Classes](data-classes/index.md)
- [Settings Classes](settings-classes/index.md)
- [Core Classes](core-classes/index.md)
- [Service Class](service-class/index.md)
- [Delegates and Events](delegates-and-events/index.md)
- [Enumerations](enumerations/index.md)
- [Not Yet Implemented and Known Gaps](not-yet-implemented-and-known-gaps.md)
- [End-to-End Example](end-to-end-example.md)
