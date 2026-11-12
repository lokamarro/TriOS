#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
mkdir -p build/tests
xcrun clang -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. app/LinuxRunner/FileTransfer.c tests/linuxrunner/file_transfer_test.c -o build/tests/file-transfer
build/tests/file-transfer
xcrun swiftc app/LinuxRunner/Environment.swift tests/linuxrunner/EnvironmentTests.swift -o build/tests/environment-tests
build/tests/environment-tests
