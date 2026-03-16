---
description: 'C# guidelines for the WSL Settings WinUI 3 application'
applyTo: 'src/windows/wslsettings/**/*.cs'
---

# C# Development — WSL Settings (WinUI 3)

The only C# code in this repo is the `wslsettings` app under `src/windows/wslsettings/`. It is a WinUI 3 desktop application using .NET 8.0, WindowsAppSDK, and the MVVM Community Toolkit.

## General Instructions

- Make only high-confidence suggestions when reviewing code changes.
- Handle edge cases and write clear exception handling.
- Match the style and patterns already used in nearby files.

## Naming Conventions

- PascalCase for class names, method names, properties, and public members.
- camelCase for private fields and local variables.
- Prefix interfaces with "I" (e.g., `INavigationService`, `IWslConfigService`).
- ViewModels: `<Feature>ViewModel` (e.g., `NetworkingViewModel`).
- Views: `<Feature>Page` (e.g., `NetworkingPage`).
- Contracts: place service interfaces under `Contracts/Services/`, ViewModel interfaces under `Contracts/ViewModels/`.

## Formatting

- Prefer file-scoped namespace declarations (all files use `namespace WslSettings.xxx;`).
- Use single-line using directives; use global usings in `Usings.cs` for common imports (e.g., `global using WinUIEx;`, `global using LibWsl;`).
- Insert a newline before the opening curly brace of any code block.
- Use pattern matching and switch expressions wherever possible.
- Use `nameof` instead of string literals when referring to member names.

## Architecture — MVVM + WinUI 3

This app follows the MVVM pattern with these conventions:

- **Views** (`Views/Settings/`, `Views/OOBE/`): XAML pages with minimal code-behind.
- **ViewModels** (`ViewModels/Settings/`, `ViewModels/OOBE/`): Use `[ObservableProperty]` and `[RelayCommand]` from Community Toolkit MVVM.
- **Services** (`Services/`): Application services registered via DI (navigation, page service, WSL config).
- **Contracts** (`Contracts/`): Interface definitions for services and view models.
- **Converters** (`Converters/`): XAML value converters (e.g., `MegabyteStringConverter`, `BooleanToVisibilityConverter`).
- **Activation** (`Activation/`): App activation handlers (protocol, default).

Key patterns:
- Use dependency injection for all services — avoid `new` for services.
- Use `[ObservableProperty]` instead of manual `INotifyPropertyChanged` boilerplate.
- Use `[RelayCommand]` for commands instead of manual `ICommand` implementations.
- Navigation is handled by `INavigationService` / `INavigationViewService`.
- WSL configuration is accessed through `IWslConfigService`.

## Nullable Reference Types

- Declare variables non-nullable, and check for `null` at entry points.
- Always use `is null` or `is not null` instead of `== null` or `!= null`.
- Trust the C# null annotations — don't add redundant null checks.

## XAML Guidelines

- Keep XAML pages focused on layout; business logic goes in ViewModels.
- Use `x:Bind` over `Binding` for compile-time safety and performance.
- Custom controls go in `Controls/` (e.g., `OOBEContent`, `HyperlinkTextBlock`).
- Localized strings live in `localization/strings/` as `.resw` resource files.

## Testing

- WSL Settings is tested as part of the full WSL test suite (TAEF framework).
- Copy existing style in nearby files for test method names and capitalization.
- Do not emit "Act", "Arrange" or "Assert" comments.
