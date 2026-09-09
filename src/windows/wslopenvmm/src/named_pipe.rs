// Copyright (C) Microsoft Corporation. All rights reserved.

use std::io;
use std::time::Duration;

use hyper_util::rt::TokioIo;
use tokio::net::windows::named_pipe::{ClientOptions, NamedPipeClient};
use tonic::transport::{Channel, Endpoint};
use tower::service_fn;
use windows::Win32::Foundation::{ERROR_FILE_NOT_FOUND, ERROR_PIPE_BUSY};

/// Connects a Tonic channel to the OpenVMM named-pipe listener.
pub async fn connect_channel(
    pipe_name: String,
    timeout: Duration,
) -> Result<Channel, tonic::transport::Error> {
    let endpoint = Endpoint::from_static("http://[::]:50051").connect_timeout(timeout);
    endpoint
        .connect_with_connector(service_fn(move |_| {
            let pipe_name = pipe_name.clone();
            async move {
                open_pipe(&pipe_name, timeout).await.map(TokioIo::new)
            }
        }))
        .await
}

async fn open_pipe(pipe_name: &str, timeout: Duration) -> io::Result<NamedPipeClient> {
    tokio::time::timeout(timeout, async {
        loop {
            match ClientOptions::new().open(pipe_name) {
                Ok(pipe) => return Ok(pipe),
                Err(error)
                    if matches!(error.raw_os_error(), Some(code)
                        if code == ERROR_FILE_NOT_FOUND.0 as i32 || code == ERROR_PIPE_BUSY.0 as i32) => {}
                Err(error) => return Err(error),
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    })
    .await
    .map_err(|_| io::Error::new(io::ErrorKind::TimedOut, "timed out connecting to OpenVMM"))?
}
