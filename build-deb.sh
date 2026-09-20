#!/bin/sh
# Build the .deb on a Debian trixie / Raspberry Pi OS (arm64) machine.
# The package lands in the parent directory: ../c-gugusse_<version>_arm64.deb
set -e
cd "$(dirname "$0")"
sudo apt-get install -y --no-install-recommends debhelper fakeroot dpkg-dev
sudo apt-get build-dep -y ./
dpkg-buildpackage -us -uc -b
