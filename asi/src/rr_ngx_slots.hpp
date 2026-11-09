// SPDX-License-Identifier: MIT
// Adapted from DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#pragma once

#if defined(__GNUC__) && defined(__x86_64__)
#define RR_NGX_MSABI __attribute__((ms_abi))
#else
#define RR_NGX_MSABI
#endif

enum : int
{
    kNgxSlotSetVoidPtr = 0,
    kNgxSlotSetD3D12 = 1,
    kNgxSlotSetD3D11 = 2,
    kNgxSlotSetInt = 3,
    kNgxSlotSetUInt = 4,
    kNgxSlotSetDouble = 5,
    kNgxSlotSetFloat = 6,
    kNgxSlotSetULL = 7,
    kNgxGetterOffset = 8,
    kNgxSlotGetVoidPtr = 8,
    kNgxSlotGetD3D12 = 9,
    kNgxSlotGetD3D11 = 10,
    kNgxSlotGetInt = 11,
    kNgxSlotGetUInt = 12,
    kNgxSlotGetDouble = 13,
    kNgxSlotGetFloat = 14,
    kNgxSlotGetULL = 15,
    kNgxSlotReset = 16,
};
