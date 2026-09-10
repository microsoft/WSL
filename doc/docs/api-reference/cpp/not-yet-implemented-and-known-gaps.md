# Not Yet Implemented and Known Gaps

| Gap | Details |
|---|---|
| Handle-based image import/load | The C API exposes `WslcImportSessionImage` and `WslcLoadSessionImage` from a `HANDLE`; the WinRT metadata exposes path-based `ImportImage` / `ImportImageAsync` and `LoadImage` / `LoadImageAsync`. |
| Explicit container start flags | `Container::Start()` takes no parameters. In `winrt_Container.cpp`, the wrapper automatically sets `WSLC_CONTAINER_START_FLAG_ATTACH` when the init process output mode is `Event` or `Stream`. |
| Raw process callback plumbing | The C API exposes `WslcSetProcessSettingsCallbacks`, `WslcGetProcessExitEvent`, and raw I/O handles. The WinRT projection hides that behind `ProcessSettings::OutputMode`, `OutputReceived`, `ErrorReceived`, `Exited`, `GetInputStream`, and `GetOutputStream`. |
| Missing wrapper source files in this drop | `winrt_CMakeLists.txt` references `PullImageOptions`, `PushImageOptions`, `TagImageOptions`, `VhdOptions`, and `ServiceVersion`|

---
