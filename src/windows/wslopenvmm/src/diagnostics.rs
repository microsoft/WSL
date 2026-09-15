// Copyright (C) Microsoft Corporation. All rights reserved.

use smallvec::SmallVec;
use std::io::Write as _;
use windows_sys::Win32::System::Diagnostics::Debug::{IsDebuggerPresent, OutputDebugStringA};

pub fn ensure_tracing_init() {
    static INIT: std::sync::Once = std::sync::Once::new();
    INIT.call_once(|| {
        use tracing_subscriber::Layer as _;
        use tracing_subscriber::layer::SubscriberExt;
        use tracing_subscriber::util::SubscriberInitExt;

        let fmt_layer = tracing_subscriber::fmt::layer()
            .with_ansi(false)
            .with_writer(DebugOutputWriter::new)
            .with_filter(tracing_subscriber::filter::filter_fn(|metadata| {
                // Avoid formatting when no debugger is listening.
                metadata.target() == "wslopenvmm::rpc"
                    && *metadata.level() <= tracing::Level::DEBUG
                    && unsafe { IsDebuggerPresent() != 0 }
            }));
        if let Err(error) = tracing_subscriber::Registry::default()
            .with(fmt_layer)
            .try_init()
        {
            let _ = writeln!(
                DebugOutputWriter::new(),
                "WSL RPC tracing initialization failed: {error}"
            );
        }
    });
}

struct DebugOutputWriter;

impl DebugOutputWriter {
    fn new() -> Self {
        Self
    }
}

impl std::io::Write for DebugOutputWriter {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        // Keep typical messages on the stack, spilling to the heap for longer output.
        let mut null_terminated: SmallVec<[u8; 1024]> = SmallVec::with_capacity(bytes.len() + 1);
        null_terminated.extend_from_slice(bytes);
        null_terminated.push(b'\0');
        // The buffer is null-terminated and valid for the duration of this call.
        unsafe { OutputDebugStringA(null_terminated.as_ptr()) };
        Ok(bytes.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}
