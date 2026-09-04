// Copyright (C) Microsoft Corporation. All rights reserved.

use std::io;
use std::time::Duration;

use hyper_util::rt::TokioIo;
use tokio::net::windows::named_pipe::ClientOptions;
use tonic::transport::{Channel, Endpoint};
use tower::service_fn;

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
                tokio::time::timeout(timeout, async move { ClientOptions::new().open(pipe_name) })
                    .await
                    .map_err(|_| {
                        io::Error::new(io::ErrorKind::TimedOut, "timed out connecting to OpenVMM")
                    })?
                    .map(TokioIo::new)
            }
        }))
        .await
}
