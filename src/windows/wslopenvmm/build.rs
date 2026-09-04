// Copyright (C) Microsoft Corporation. All rights reserved.

use std::env;
use std::error::Error;
use std::path::PathBuf;

fn main() -> Result<(), Box<dyn Error>> {
    let proto =
        PathBuf::from(env::var_os("VM_SERVICE_PROTO").ok_or("VM_SERVICE_PROTO is not set")?);
    let proto_directory = proto
        .parent()
        .ok_or("VM_SERVICE_PROTO has no parent directory")?
        .to_path_buf();
    let protoc = protoc_bin_vendored::protoc_bin_path()?;
    let protoc_include = protoc_bin_vendored::include_path()?;

    println!("cargo::rerun-if-env-changed=VM_SERVICE_PROTO");
    println!("cargo::rerun-if-changed={}", proto.display());
    unsafe {
        env::set_var("PROTOC", protoc);
    }

    tonic_prost_build::configure().compile_protos(&[proto], &[proto_directory, protoc_include])?;
    Ok(())
}
