# Common CsWinRT Type Mappings

| WinRT type | C# projection |
|---|---|
| `hstring` | `string` |
| `Windows.Foundation.Uri` | `System.Uri` |
| `Windows.Foundation.TimeSpan` | `System.TimeSpan` |
| `IReference<uint32_t>` | `uint?` |
| `IReference<TimeSpan>` | `TimeSpan?` |
| `IReference<ContainerNetworkingMode>` | `ContainerNetworkingMode?` |
| `IVector<T>` | `IList<T>` |
| `IVectorView<T>` | `IReadOnlyList<T>` |
| `IMap<string, string>` | `IDictionary<string, string>` |
| `Windows.Foundation.DateTime` | `DateTimeOffset` |
| `com_array<uint8_t>` event payload | `byte[]` |
| `Windows.Networking.HostName` | `Windows.Networking.HostName` |
| `IAsyncActionWithProgress<T>` | awaitable WinRT async operation |

---
