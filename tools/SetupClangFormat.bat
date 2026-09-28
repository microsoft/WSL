@ECHO OFF
rem Copyright (c) Microsoft Corporation.
rem Licensed under the MIT License.

rem This script sets up git hooks for clang-format

pushd %~dp0%
git config --local core.hooksPath tools/hooks
popd