#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
wayland-scanner client-header ext-data-control-v1.xml ext-data-control-v1-client-protocol.h
wayland-scanner private-code ext-data-control-v1.xml ext-data-control-v1-protocol.c
