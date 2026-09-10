# WSL security model

WSL provides a Linux environment that is deeply integrated with Windows. It is not a security sandbox for running untrusted code.

## Trust boundaries

Code running in WSL should be treated as having the same trust level as code running under the Windows account that launched it. A
WSL distribution does not isolate malicious or untrusted workloads from the Windows host or from other distributions running for
the same user.

The Linux `root` user has full control of its distribution and can change its configuration. Linux `root` is not the same as a
Windows administrator, but the distribution is not a security boundary from the associated Windows user.

Documented Windows security boundaries and security features continue to apply. Suspected violations of those boundaries or
features should be reported to MSRC for assessment.

## Configuration settings

WSL configuration settings control product behavior. Unless a setting is explicitly documented as a security control, do not use
it as a containment boundary.

For example:

- `[interop] enabled=false` in `/etc/wsl.conf` disables the normal workflow for launching Windows processes from that distribution.
- `[automount] enabled=false` in `/etc/wsl.conf` disables automatic mounting of Windows drives.
- `.wslconfig` settings configure the WSL 2 virtual machine and its features.

These settings change the available integration features, but they do not turn a distribution into a sandbox or isolate it from
Windows resources available to the user.

## Untrusted workloads and containers

To isolate untrusted code from the Windows host, use a security boundary designed for that purpose, such as a dedicated virtual
machine with appropriately restricted access.

Containers running inside WSL inherit the security properties of their container runtime configuration and WSL. Running a workload
in a container does not make WSL a security boundary from the Windows host.

For the criteria used to evaluate Windows security reports, see the
[Microsoft Security Servicing Criteria for Windows](https://www.microsoft.com/msrc/windows-security-servicing-criteria).
