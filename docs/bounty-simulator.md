# WSL Bounty Simulator

This document provides guidance for simulating bounty scenarios in the WSL development environment.

## Overview

The bounty simulator is designed to help developers test and validate bounty-related functionality within WSL. This includes testing various scenarios that might occur during bounty collection and processing.

## Prerequisites

Before using the bounty simulator, ensure you have:

- WSL 2 installed and configured
- Ubuntu 16.04 or later distribution
- Kernel version 5.4.72 or compatible
- Administrative privileges on Windows

## Setup

### 1. Environment Configuration

Verify your WSL installation:

```bash
wsl -l -v
```

Check your Windows version:

```cmd
cmd.exe /c ver
```

### 2. Required Dependencies

Install necessary packages in your WSL distribution:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake
```

## Usage

### Basic Simulation

To run a basic bounty simulation:

1. Navigate to the WSL project directory
2. Build the project using CMake:
   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```

### Advanced Scenarios

The simulator supports various test scenarios:

- **Network connectivity tests**: Simulate bounty collection under different network conditions
- **Resource constraint tests**: Test behavior under memory and CPU limitations
- **Error handling tests**: Validate proper error handling and recovery

### Configuration Options

You can customize the simulator behavior by modifying the `UserConfig.cmake.sample` file:

```cmake
# Copy and modify as needed
cp UserConfig.cmake.sample UserConfig.cmake
```

## Troubleshooting

### Common Issues

1. **Build failures**: Ensure all dependencies are installed and CMake is properly configured
2. **Permission errors**: Verify you have appropriate permissions in both Windows and WSL
3. **Network issues**: Check WSL networking configuration if bounty simulation involves network operations

### Collecting Logs

If you encounter issues, collect diagnostic logs using the WSL log collection script:

```powershell
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/collect-wsl-logs.ps1" -OutFile collect-wsl-logs.ps1
Set-ExecutionPolicy Bypass -Scope Process -Force
.\collect-wsl-logs.ps1
```

## Contributing

When contributing bounty simulator improvements:

1. Follow the guidelines in [CONTRIBUTING.md](../CONTRIBUTING.md)
2. Include appropriate test cases
3. Update documentation as needed
4. Ensure compatibility with both WSL 1 and WSL 2

## Support

For issues related to the bounty simulator:

1. Check existing issues in the [WSL repository](https://github.com/microsoft/WSL/issues)
2. Provide detailed reproduction steps
3. Include diagnostic logs when filing issues
4. Follow the issue template guidelines

## Security Considerations

- Do not include sensitive information in bounty simulations
- Follow secure coding practices
- Report security issues to secure@microsoft.com rather than public issue trackers

## See Also

- [Developer Loop Documentation](./dev-loop.md)
- [WSL Architecture Overview](./architecture.md)
- [Contributing Guidelines](../CONTRIBUTING.md)
