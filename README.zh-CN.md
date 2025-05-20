# 欢迎来到适用于 Linux 的 Windows 子系统 (WSL) 存储库

<p align="center">
  <img src="./Images/Square44x44Logo.targetsize-256.png" alt="WSL 徽标"/>
</p>

[了解更多关于 WSL](https://aka.ms/wsldocs) | [下载和发行说明](https://github.com/microsoft/WSL/releases) | [为 WSL 做贡献](./CONTRIBUTING.md)

## 关于

适用于 Linux 的 Windows 子系统 (WSL) 是一种功能强大的方法，可让您直接在 Windows 上运行 Linux 命令行工具、实用程序和应用程序，所有这些都无需修改，也无需传统虚拟机或双引导设置的开销。

您可以立即安装 WSL，方法是在 Windows 命令行中运行以下命令：

```powershell
wsl --install
```

您可以了解更多关于[设置的最佳实践](https://learn.microsoft.com/windows/wsl/setup/environment)、[WSL 概述](https://learn.microsoft.com/windows/wsl/about)以及更多信息，请访问我们的 [WSL 文档页面](https://learn.microsoft.com/windows/wsl/)。

## 相关存储库

WSL 还有相关的开源存储库：

- [microsoft/WSL2-Linux-Kernel](https://github.com/microsoft/WSL2-Linux-Kernel) - WSL 附带的 Linux 内核
- [microsoft/WSLg](https://github.com/microsoft/wslg) - 支持 WSL 中的 Linux GUI 应用程序
- [microsoftdocs/wsl](https://github.com/microsoftdocs/wsl) - WSL 文档位于 aka.ms/wsldocs

## 贡献

本项目欢迎所有类型的贡献，包括编码功能/错误修复、文档修复、设计提案等。

我们要求您在开始贡献之前，请阅读我们的[贡献者指南](./CONTRIBUTING.md)。

有关为 WSL 开发的指导，请阅读[开发人员文档](./doc/docs/dev-loop.md)，了解如何从源代码构建 WSL 及其体系结构的详细信息。

## 行为准则

本项目采用了[微软开源行为准则](./CODE_OF_CONDUCT.md)

## 商标

本项目可能包含项目、产品或服务的商标或徽标。微软商标或徽标的授权使用受[微软商标和品牌指南](https://www.microsoft.com/legal/intellectualproperty/trademarks)的约束，并且必须遵守该指南。在本项目的修改版本中使用微软商标或徽标不得引起混淆或暗示微软赞助。任何第三方商标或徽标的使用均受这些第三方政策的约束。

## 隐私和遥测

该应用程序记录基本的诊断数据（遥测）。有关隐私和我们收集的内容的更多信息，请参阅我们的[数据和隐私文档](DATA_AND_PRIVACY.md)。

该软件可能会收集有关您以及您对软件使用的信息，并将其发送给微软。微软可能会使用这些信息来提供服务并改进我们的产品和服务。您可以按照存储库中的说明关闭遥测。软件中还有一些功能可能使您和微软能够从您的应用程序用户那里收集数据。如果您使用这些功能，则必须遵守适用的法律，包括向您的应用程序用户提供适当的通知以及微软隐私声明的副本。我们的隐私声明位于 https://go.microsoft.com/fwlink/?LinkID=824704。您可以了解更多关于数据收集和使用的信息，请参阅帮助文档和我们的隐私声明。您对软件的使用即表示您同意这些做法。
