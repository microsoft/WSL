# Systemd

Systemd support for a WSL distribution can be enabled by setting the following in `/etc/wsl.conf`:

```
[boot]
systemd=true
```

When enabled, [init](init.md) will launch `/sbin/init` (which points to systemd's init) when the distribution starts. One key difference when this setting is enabled is that [init](init.md) won't be pid 1 in the given distribution, since systemd's init requires running as pid 1, so [init](init.md) will fork(), and launch systemd in the parent while continuing WSL configuration in the child process. 

After launching `/sbin/init`, [init](init.md) waits for systemd to be ready by waiting for `systemctl is-system-running` to return either `running`, or `degraded`. After a given amount of time, WSL will time out and allow the distribution to continue starting, even if systemd isn't ready.

## User sessions

When systemd is enabled, WSL tries synchronizes launching processes with systemd user sessions. This is currently done by launching `login -f <user>` to start the associated systemd user session.

## Additional systemd configuration 

To improve compatibility with systemd, WSL creates various systemd configuration files during boot (under `/run`). These configurations files are used to:

- Protect the WSL [binfmt interpreter](interop.md) from being deleted by `systemd-binfmt.service`
- Protect the X11 socket from being deleted by `systemd-tmpfiles-setup.service`
