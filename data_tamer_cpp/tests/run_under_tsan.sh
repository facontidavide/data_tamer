#!/usr/bin/env bash
# ThreadSanitizer needs a smaller ASLR range than recent kernels use by
# default; -R disables address-space randomisation for this process only.
# The suppression file covers fence-based ordering in the vendored queue.
export TSAN_OPTIONS="${TSAN_OPTIONS:+${TSAN_OPTIONS}:}suppressions=$(dirname "$(readlink -f "$0")")/tsan.supp"
exec setarch "$(uname -m)" -R "$@"
