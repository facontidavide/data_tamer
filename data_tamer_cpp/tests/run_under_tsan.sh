#!/usr/bin/env bash
# ThreadSanitizer needs a smaller ASLR range than recent kernels use by
# default; -R disables address-space randomisation for this process only.
exec setarch "$(uname -m)" -R "$@"
