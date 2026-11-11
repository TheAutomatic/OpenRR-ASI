# Upstream v0.1.4 integration — 2026-11-11

Integrated upstream `fc61896` (v0.1.4) onto ASI commit `516ced2` on
`codex/integrate-014`. This is a merge preserving published ancestry; the
original `asi-plugin` branch remains at `516ced2`.

## Resolution choices

- Adopt upstream lifetime-based Fast-mode arena and LT_VO preprojected weights.
  Remove the superseded local scratch-tail pool and its test-only controls.
- Retain local batched initialization, larger flow buffer required by the local
  flow shader, post optimizations, and GPU completion/retirement protections.
- Allocate wide attributes separately instead of relocating arena views. This
  consumes additional memory when wide guides are used, but preserves arena
  lifetime assumptions.
- Set the upstream builder cancellation flag before the local teardown joins
  builders; retain deferred GPU-safe reclamation.
- Preserve validator reset notifications in the upstream CPU-edit refactor.
- Keep ASI, Streamline adaptation, DXGI queue association and Present scope fixes.

## Validation

Hardware: RX 9070 XT, driver 26.10.1 (LLPC), DLSSD 310.9.1.0.

- `wsl --exec bash asi/build/build_asi.sh build/integrate-014`: PASS, 90 SPIR-V
  shaders, ASI 3,235,092 bytes. Build log: `build/integrate-014/rebuild.log`.
- Final rebuilt `test_asi_units.exe`: exit 0, ALL ASI UNIT TESTS PASSED.
  Log: `build/integrate-014/unit-final.log`. Includes Fast/Legacy and DRS,
  finite output, 1080p/4K exact repeat over 20-frame sequences with wide guides,
  shader hash parity, D3D12 slot saturation/reset/replay, D3D12/Vulkan bridge,
  native Vulkan RecordVk and retirement churn, main-view Create/immediate
  Destroy before host device destruction, and module-relative shader discovery.
  The immediate-destroy check exercises builder startup/cancellation; it does
  not establish cancellation latency during a cold driver pipeline compilation.
- `test_present_scope.exe` and `test_dxgi_queue.exe`: exit 0. Cross-thread
  Present, unrelated DIRECT submission, recreation, ResizeBuffers, composition
  and legacy creation, plus nested Present/Present1 scope checks passed.
- D3D12 validator compiled and linked with MinGW (`-municode`); Vulkan
  validator passed MinGW syntax compilation. No NGX-reference capture comparison
  was run for this integration.
- Godot D3D12 1920x1080: 30 frames, 32 presents, 10 RR network frames, normal
  shutdown. `build/integrate-014/godot-d3d12`.
- Godot Vulkan renderer 1280x720: 120 frames, 122 presents, 88 RR network frames,
  normal shutdown. `build/integrate-014/godot-vulkan-long`. Its consumer/RR
  remains D3D12; native Vulkan coverage is the separate unit suite above. The
  first 30-frame Vulkan-renderer run ended before RR became ready and is not
  counted as inference validation.
- Both Godot scenarios used official OptiScaler v10 pre1 with FG disabled.
  The actual loaded path was `runtime/OptiScaler/plugins/openrr-amd.asi`;
  logs confirm v0.1.4 and menu drawing over a D3D12 swap chain.

Built/tested ASI SHA-256:
`0174C795EB225AF5C36DB95860C7DA1FC913B20334F5A25C3C395E1B528CFCBA`.
Build stamp is the pre-merge HEAD `516ced2`; runtime version is 0.1.4.

These are correctness and smoke checks, not a controlled performance comparison
against the previous ASI. LT_VO changes Fast-mode arithmetic, so exact parity
with the previous version is not asserted. No claim of universal game coverage,
OptiFG validation, or actual Talos 2 gameplay is made by this integration run.
