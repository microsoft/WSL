# Known Gaps

These C API features are **not** available through the WinRT/C# projection:

| C API feature | C# status |
|---|---|
| `WslcRegisterSessionCrashDumpCallback`, `WslcReleaseCrashDumpSubscription`, `WslcSessionCrashDumpInfo` | **Not projected.** Crash dump callbacks are available only in the native C API. |
| `WslcImportSessionImage(...)` and `WslcLoadSessionImage(...)` overloads that take raw `HANDLE` + byte count | **Not projected.** C# only exposes file-path-based `ImportImageAsync(string path, string imageName)` and `LoadImageAsync(string path)`. |
| Raw native handles (`WslcGetSessionTerminationEvent`, `WslcGetProcessExitEvent`, `WslcGetProcessIOHandle`) | **Wrapped, not exposed directly.** Use C# events and WinRT streams instead. |
| `WslcProcessCallbacks` registration surface | **Wrapped as events.** Use `OutputReceived`, `ErrorReceived`, and `Exited`. |
| `WslcContainerStartFlags` | **Not exposed directly.** `Container.Start()` automatically sets `ATTACH` when the init process uses `ProcessOutputMode.Event` or `ProcessOutputMode.Stream`. |

---
