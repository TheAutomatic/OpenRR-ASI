// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

struct ID3D12GraphicsCommandList;
struct OnrRrFrame;

namespace openrr::pe {

// The embedded runtime tells the ASI whether it changed the submitted host list's
// compute state. Private uploads can run earlier on a different list.
int32_t record_d3d12(void* context, const OnrRrFrame* frame, ID3D12GraphicsCommandList* list,
                     bool& host_bound, bool trace);

}  // namespace openrr::pe
