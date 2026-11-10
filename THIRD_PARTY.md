# Third-Party Notices and Licenses

The offline source-level ACO address32 capture strategy in
asi/build/mesa/prepare_capture.py and the dual-capture validation in
asi/build/build_aco_records.py are adapted from the OpenNR record producer in
neural-amd-opti-reference/tools/aco-records/mesa-25.3.6-nr-record.patch and
make_record.py. That reference project is licensed under GPL-3.0; these offline
build-tool adaptations retain that provenance and are not linked into the
Windows runtime. See the reference project's LICENSE when redistributing these
tools or a compiler built with the adapted patch.

OpenRR (`openrr-amd.asi`) incorporates or adapts the following third-party software components:

---

## 1. DLSSNR-AMD ASI Shell (`asi/src/rr_*.cpp`, `asi/src/rr_*.hpp`, `asi/build/`)

Adapted from `DLSSNR-AMD` (`windows/src/pe/`), Copyright (c) 2026 mochizuki0323.
- The PAL ELF, MessagePack, NRPAL002 parser and prologue in `gpu/src/rr_pal_binary.hpp` are adapted from `windows/src/core/nr_pal_binary.hpp` of the same project under the MIT license below.
- `asi/src/rr_ngx_abi.hpp` and `asi/src/rr_ngx_slots.hpp` are clean-room ABI declarations transcribed from NVIDIA NGX SDK headers (`nvsdk_ngx.h`, `nvsdk_ngx_defs.h`, `nvsdk_ngx_defs_dlssd.h`, `nvsdk_ngx_params.h`, `nvsdk_ngx_vk.h`) for binary interoperability without NVIDIA SDK header dependencies.

```text
MIT License

Copyright (c) 2026 mochizuki0323

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 2. Dear ImGui (`asi/third_party/imgui/`)

Full license text in `asi/third_party/imgui/LICENSE.txt`.

```text
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 3. MinHook (`asi/third_party/minhook/`)

Full license text in `asi/third_party/minhook/LICENSE.txt`.

```text
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## 4. Inter Font (`asi/third_party/inter/`)

Copyright (c) 2016-2024 The Inter Project Authors. Licensed under the SIL Open Font License, Version 1.1.
Full license text in `asi/third_party/inter/LICENSE.txt`.

---

## 5. Khronos Vulkan-Headers (`asi/third_party/vulkan/`)

Copyright (c) 2015-2025 The Khronos Group Inc. Licensed under the Apache License, Version 2.0.
Full license text in `asi/third_party/vulkan/LICENSE.md`.

---

## 6. Mesa RADV / ACO (offline build tool)

Offline shader records are generated with Mesa 26.2.4, pinned to
`96cb43121031992b85767f9c1be8f3f48e22b1d2`, with the source patches in
`asi/build/mesa/`. Mesa RADV/ACO sources used here retain their upstream MIT
license notices. The package includes generated shader records; Mesa shared
libraries, the noop DRM shim and the capture executable are build tools and
are not distributed in the Windows stage. See `asi/build/mesa/README.md` for
patch provenance and reproducible build instructions.
