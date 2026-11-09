// SPDX-License-Identifier: MIT
// Adapted from DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#pragma once
#include <d3d12.h>
#ifdef __MINGW32__
typedef HRESULT(WINAPI* PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(const D3D12_ROOT_SIGNATURE_DESC*,
                                                            D3D_ROOT_SIGNATURE_VERSION,
                                                            ID3DBlob**, ID3DBlob**);
#endif

extern HMODULE dllModule;
struct ID3D12PipelineLibrary;
