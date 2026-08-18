# Referenced

- `Session::PullImageAsync(PullImageOptions)` forwards a struct whose C shape is `{ uri, progressCallback, progressCallbackContext, registryAuth }`.
- `Session::PushImageAsync(PushImageOptions)` forwards a struct whose C shape is `{ image, registryAuth, progressCallback, progressCallbackContext }`.
- `Session::TagImage(TagImageOptions)` forwards a struct whose C shape is `{ image, repo, tag }`.
- `Session::CreateVhdVolume(VhdOptions)` and `SessionSettings::VhdRequirements(VhdOptions)` use `VhdOptions` properties `Name`, `Size`, `Type`, and `Owner`.
- `WslcService::GetVersion()` returns a `ServiceVersion` created from C `major`, `minor`, and `revision` values.



```cpp
PullImageOptions pullOptions = /* construct using the wrapper available in your build */;
auto pull = session.PullImageAsync(pullOptions);
pull.Progress([](auto&&, ImageProgress const& p) { /* ... */ });
co_await pull;

PushImageOptions pushOptions = /* construct using the wrapper available in your build */;
co_await session.PushImageAsync(pushOptions);

TagImageOptions tagOptions = /* construct using the wrapper available in your build */;
session.TagImage(tagOptions);

VhdOptions vhdOptions = /* construct using the wrapper available in your build */;
session.CreateVhdVolume(vhdOptions);

auto version = WslcService::GetVersion();
(void)version;
```

---
