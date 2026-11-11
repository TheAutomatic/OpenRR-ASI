# D3D12 menu queue association validation

System: Windows, Radeon RX 9070 XT. Full ASI build: MinGW, build/menu-queue-review.
ASI SHA256: 7660AF8F7769915158E23A317D2A2745C279AEA3C0F7A1E8522381EBA1320204.

## Passed

- test_dxgi_queue.exe: exact queue identity on the creating and a separate Present thread, after unrelated DIRECT queue submission, across three swap-chain generations and ResizeBuffers.
- CreateSwapChain, CreateSwapChainForHwnd, CreateSwapChainForComposition: successful creation and exact queue identity. All four factory method hooks install successfully; CoreWindow creation is not exercised by this desktop probe.
- OptiScaler 10.0.0-pre1 (20261109): probe passes with OpenRR factory hooks installed both before and after the proxy factory is created. OptiScaler logs confirm HookToFactory and WrappedIDXGISwapChain4 execution.
- Same probe named talos2.exe: passes with OptiScaler. This is an executable-name probe, not the actual game or an OptiPatcher integration test.
- Godot engine D3D12, 1280x720, 30 frames: normal exit, RR feature creation/release and D3D12 menu drawing logged.
- Godot engine D3D12, 1920x1080, 30 frames, OptiScaler 10 pre1: same result.
- Godot engine Vulkan, 1280x720, 30 frames, OptiScaler 10 pre1: same result. The companion consumer and RR run on D3D12; this does not validate native Vulkan RR.
- git diff --check.

## Test setup corrections and limits

Initial crashes loaded a stale ASI from runtime/OptiScaler/plugins, while the test deployment replaced runtime/plugins. Those runs are invalid comparisons. Corrected runs verify the actual LoadAsiPlugins path and the loaded file's SHA256 against the build.

The old test venv could not launch its configured WindowsApps interpreter from the execution environment. Tests used the bundled Python runtime without changing the machine's Python installation or environment. A local launcher suppresses OS crash dialogs and retries transient PermissionError when reading IPC packets; the first Vulkan run hit that IPC race, and its retry passed.

Probe output and Godot logs remain in build/menu-queue-review (ignored build artifacts). ResizeBuffers1 queue replacement is handled in code; multi-node per-buffer distinct queues deliberately disable cached single-queue drawing and have not been exercised on this single-GPU setup.
