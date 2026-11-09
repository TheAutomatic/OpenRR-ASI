# OpenRR Standalone Steady + DRS Benchmark & Bit-Exact Parity Harness (`openrr_bench.exe`)

Single-file D3D12 benchmark and bit-exact FP16 output comparator ([`openrr_bench.cpp`](openrr_bench.cpp)) used to measure steady-state GPU frame time, Dynamic Resolution Scaling (DRS) GPU frame time, feature/process VRAM delta, and 100% bit-exact output parity (`max_ulp_diff == 0`) between any baseline `OpenNrRrRuntime.dll` and candidate `openrr-amd.asi` / `OpenNrRrRuntime.dll`.

## Build

`bash asi/build/build_asi.sh` automatically builds `build/asi/openrr_bench.exe` via MinGW-w64 (`x86_64-w64-mingw32-g++`).

## Run Examples

```powershell
# 1. Compare upstream OpenRR baseline vs openrr-amd.asi (1440p Quality 1706x960 -> 2560x1440, Legacy mode):
.\build\asi\openrr_bench.exe `
  --baseline-runtime C:\Users\Kenny\Downloads\dlssnr_on_amd_setup\exports\openrr-baseline\OpenNrRrRuntime.dll `
  --baseline-data-dir C:\Users\Kenny\Downloads\dlssnr_on_amd_setup\exports\openrr-runtime\opennr-rr `
  --runtime build\asi\stage\plugins\openrr-amd.asi `
  --data-dir build\asi\stage\openrr-amd `
  --dlssd C:\Users\Kenny\Downloads\dlssnr_on_amd_setup\dist\weights\nvngx_dlssd.dll `
  --out build\asi\bench-vs-upstream --fast-mode 0

# 2. Compare main branch OpenNrRrRuntime.dll vs openrr-amd.asi (1440p Quality, Fast mode):
.\build\asi\openrr_bench.exe `
  --baseline-runtime C:\Users\Kenny\Downloads\dlssnr_on_amd_setup\exports\openrr-runtime\OpenNrRrRuntime.dll `
  --baseline-data-dir C:\Users\Kenny\Downloads\dlssnr_on_amd_setup\exports\openrr-runtime\opennr-rr `
  --runtime build\asi\stage\plugins\openrr-amd.asi `
  --data-dir build\asi\stage\openrr-amd `
  --dlssd C:\Users\Kenny\Downloads\dlssnr_on_amd_setup\dist\weights\nvngx_dlssd.dll `
  --out build\asi\bench-1440p --fast-mode 1

# 3. Compare main branch OpenNrRrRuntime.dll vs openrr-amd.asi (4K Quality 2560x1440 -> 3840x2160, Fast mode):
.\build\asi\openrr_bench.exe `
  --baseline-runtime C:\Users\Kenny\Downloads\dlssnr_on_amd_setup\exports\openrr-runtime\OpenNrRrRuntime.dll `
  --baseline-data-dir C:\Users\Kenny\Downloads\dlssnr_on_amd_setup\exports\openrr-runtime\opennr-rr `
  --runtime build\asi\stage\plugins\openrr-amd.asi `
  --data-dir build\asi\stage\openrr-amd `
  --dlssd C:\Users\Kenny\Downloads\dlssnr_on_amd_setup\dist\weights\nvngx_dlssd.dll `
  --out build\asi\bench-4k --rw 2560 --rh 1440 --ow 3840 --oh 2160 --fast-mode 1
```
