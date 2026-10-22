# Copyright (C) Microsoft Corporation. All rights reserved.

function Get-PayloadFileSha256([string]$Path)
{
    $sha = [Security.Cryptography.SHA256]::Create()
    try
    {
        $stream = [IO.File]::OpenRead($Path)
        try
        {
            return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
        }
        finally
        {
            $stream.Dispose()
        }
    }
    finally
    {
        $sha.Dispose()
    }
}
