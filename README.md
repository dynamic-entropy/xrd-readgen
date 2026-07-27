# xrd-readgen

XRootD (`root://`) remote-read traffic generator for CMS / WLCG data challenges.

Generates controlled, measured remote-read load against XRootD endpoints (AAA
federation redirectors or individual servers).

Client-side metrics (open latency, TTFB, per-op latency, redirect depth, error classes)
are pushed to a Prometheus Pushgateway and/or written as result files.

## Build

Requires CMake >= 3.24, a C++17 compiler, and XRootD client libraries
(`brew install xrootd` / EPEL `xrootd-client-devel`, or point `-DXRootD_DIR`
at a local build tree).

```sh
cmake -S . -B build
cmake --build build -j
```

## Quick start

```sh
# terminal 1: throwaway local server (creates a 256 MiB test file)
dev/local-server.sh

# terminal 2: timed remote read
build/xrd-readgen read root://localhost:10945//tmp/xrd-readgen-data/test-256M.bin
build/xrd-readgen read --vector 16 --chunk-size 131072 root://localhost:10945//tmp/xrd-readgen-data/test-256M.bin
```

Against the grid (x509 proxy):

```sh
voms-proxy-init -voms cms
build/xrd-readgen read root://cms-xrd-global.cern.ch//store/<filename_lfn>
```

All traffic is tagged `XRD_APPNAME=xrd-readgen/<version>` so server-side
monitoring (MONIT) can identify and filter it.
