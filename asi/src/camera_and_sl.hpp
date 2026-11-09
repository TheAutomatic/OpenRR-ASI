// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>
#include "MinHook.h"
#include "rr_ngx_abi.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

namespace openrr {

// Minimal Streamline ABI declarations matching sl_struct.h and sl_consts.h (kStructVersion2).
namespace sl {

constexpr float INVALID_FLOAT = 3.402823466e+38F;

enum class Result : uint32_t {
    eOk = 0,
};

enum Boolean : char {
    eFalse = 0,
    eTrue = 1,
    eInvalid = 2,
};

struct StructType {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct BaseStructure {
    BaseStructure* next{};
    StructType structType{};
    size_t structVersion{};
};

struct float2 { float x, y; };
struct float3 { float x, y, z; };
struct float4 { float x, y, z, w; };
struct float4x4 {
    float4 row[4];
    const float4& operator[](size_t i) const { return row[i]; }
    float4& operator[](size_t i) { return row[i]; }
};

struct FrameToken;
struct ViewportHandle { uint32_t value = 0; };

struct Constants : BaseStructure {
    float4x4 cameraViewToClip{};
    float4x4 clipToCameraView{};
    float4x4 clipToLensClip{};
    float4x4 clipToPrevClip{};
    float4x4 prevClipToClip{};

    float2 jitterOffset{};
    float2 mvecScale{};
    float2 cameraPinholeOffset{};
    float3 cameraPos{};
    float3 cameraUp{};
    float3 cameraRight{};
    float3 cameraFwd{};

    float cameraNear = INVALID_FLOAT;
    float cameraFar = INVALID_FLOAT;
    float cameraFOV = INVALID_FLOAT;
    float cameraAspectRatio = INVALID_FLOAT;
    float motionVectorsInvalidValue = INVALID_FLOAT;

    Boolean depthInverted = Boolean::eInvalid;
    Boolean cameraMotionIncluded = Boolean::eInvalid;
    Boolean motionVectors3D = Boolean::eInvalid;
    Boolean reset = Boolean::eInvalid;
    Boolean orthographicProjection = Boolean::eFalse;
    Boolean motionVectorsDilated = Boolean::eFalse;
    Boolean motionVectorsJittered = Boolean::eFalse;

    float minRelativeLinearDepthObjectSeparation = 40.0f;
};

} // namespace sl

class SlCameraTracker {
public:
    static SlCameraTracker& Instance() {
        static auto* inst = new SlCameraTracker();
        return *inst;
    }

    void TryHookLoadedModule() {
        HMODULE mod = GetModuleHandleW(L"sl.interposer.dll");
        if (!mod) return;
        TryHookModule(mod);
    }

    void TryHookModule(HMODULE mod) {
        if (!mod) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (hookedMod_ == mod) return;
        void* target = reinterpret_cast<void*>(GetProcAddress(mod, "slSetConstants"));
        if (!target) return;

        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;

        PFN_slSetConstants orig = nullptr;
        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedSlSetConstants),
                          reinterpret_cast<void**>(&orig)) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            o_slSetConstants_ = orig;
            hookedMod_ = mod;
            hooked_.store(true, std::memory_order_release);
        }
    }

    bool GetSnapshot(sl::Constants& out) const {
        if (!hasConstants_.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::mutex> lock(snapMu_);
        out = lastConstants_;
        return true;
    }

private:
    using PFN_slSetConstants = sl::Result(RR_NGX_MSABI*)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);

    static sl::Result RR_NGX_MSABI HookedSlSetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) {
        auto& self = Instance();
        {
            std::lock_guard<std::mutex> lock(self.snapMu_);
            self.lastConstants_ = values;
            self.hasConstants_.store(true, std::memory_order_release);
        }
        return self.o_slSetConstants_ ? self.o_slSetConstants_(values, frame, viewport) : sl::Result::eOk;
    }

    std::mutex mu_;
    HMODULE hookedMod_ = nullptr;
    PFN_slSetConstants o_slSetConstants_ = nullptr;
    std::atomic<bool> hooked_{false};

    mutable std::mutex snapMu_;
    sl::Constants lastConstants_{};
    std::atomic<bool> hasConstants_{false};
};

namespace camera {

constexpr float kPi = 3.14159265358979323846f;

struct Mat4 {
    float m[4][4]{};
};

inline Mat4 MakeMat4(
    float m00, float m01, float m02, float m03,
    float m10, float m11, float m12, float m13,
    float m20, float m21, float m22, float m23,
    float m30, float m31, float m32, float m33) {
    Mat4 out{};
    out.m[0][0] = m00; out.m[0][1] = m01; out.m[0][2] = m02; out.m[0][3] = m03;
    out.m[1][0] = m10; out.m[1][1] = m11; out.m[1][2] = m12; out.m[1][3] = m13;
    out.m[2][0] = m20; out.m[2][1] = m21; out.m[2][2] = m22; out.m[2][3] = m23;
    out.m[3][0] = m30; out.m[3][1] = m31; out.m[3][2] = m32; out.m[3][3] = m33;
    return out;
}

inline bool IsInitialized(float v) {
    return std::isfinite(v) && v != sl::INVALID_FLOAT;
}

inline bool MatrixIsInitialized(const Mat4& matrix) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!IsInitialized(matrix.m[r][c]))
                return false;
    return true;
}

inline Mat4 Transpose(const Mat4& a) {
    Mat4 out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out.m[r][c] = a.m[c][r];
    return out;
}

inline Mat4 LoadFloat4x4(const float (&arr)[16]) {
    Mat4 out{};
    std::memcpy(out.m, arr, sizeof(arr));
    return out;
}

inline void StoreFloat4x4(float (&arr)[16], const Mat4& mat) {
    std::memcpy(arr, mat.m, sizeof(arr));
}

inline bool TryInvert(const Mat4& matrix, Mat4& inverse) {
    if (!MatrixIsInitialized(matrix))
        return false;
    const float* m = &matrix.m[0][0];
    float inv[16];

    inv[0] = m[5]  * m[10] * m[15] -
             m[5]  * m[11] * m[14] -
             m[9]  * m[6]  * m[15] +
             m[9]  * m[7]  * m[14] +
             m[13] * m[6]  * m[11] -
             m[13] * m[7]  * m[10];

    inv[4] = -m[4]  * m[10] * m[15] +
              m[4]  * m[11] * m[14] +
              m[8]  * m[6]  * m[15] -
              m[8]  * m[7]  * m[14] -
              m[12] * m[6]  * m[11] +
              m[12] * m[7]  * m[10];

    inv[8] = m[4]  * m[9] * m[15] -
             m[4]  * m[11] * m[13] -
             m[8]  * m[5] * m[15] +
             m[8]  * m[7] * m[13] +
             m[12] * m[5] * m[11] -
             m[12] * m[7] * m[9];

    inv[12] = -m[4]  * m[9] * m[14] +
               m[4]  * m[10] * m[13] +
               m[8]  * m[5] * m[14] -
               m[8]  * m[6] * m[13] -
               m[12] * m[5] * m[10] +
               m[12] * m[6] * m[9];

    inv[1] = -m[1]  * m[10] * m[15] +
              m[1]  * m[11] * m[14] +
              m[9]  * m[2] * m[15] -
              m[9]  * m[3] * m[14] -
              m[13] * m[2] * m[11] +
              m[13] * m[3] * m[10];

    inv[5] = m[0]  * m[10] * m[15] -
             m[0]  * m[11] * m[14] -
             m[8]  * m[2] * m[15] +
             m[8]  * m[3] * m[14] +
             m[12] * m[2] * m[11] -
             m[12] * m[3] * m[10];

    inv[9] = -m[0]  * m[9] * m[15] +
              m[0]  * m[11] * m[13] +
              m[8]  * m[1] * m[15] -
              m[8]  * m[3] * m[13] -
              m[12] * m[1] * m[11] +
              m[12] * m[3] * m[9];

    inv[13] = m[0]  * m[9] * m[14] -
              m[0]  * m[10] * m[13] -
              m[8]  * m[1] * m[14] +
              m[8]  * m[2] * m[13] +
              m[12] * m[1] * m[10] -
              m[12] * m[2] * m[9];

    inv[2] = m[1]  * m[6] * m[15] -
             m[1]  * m[7] * m[14] -
             m[5]  * m[2] * m[15] +
             m[5]  * m[3] * m[14] +
             m[13] * m[2] * m[7] -
             m[13] * m[3] * m[6];

    inv[6] = -m[0]  * m[6] * m[15] +
              m[0]  * m[7] * m[14] +
              m[4]  * m[2] * m[15] -
              m[4]  * m[3] * m[14] -
              m[12] * m[2] * m[7] +
              m[12] * m[3] * m[6];

    inv[10] = m[0]  * m[5] * m[15] -
              m[0]  * m[7] * m[13] -
              m[4]  * m[1] * m[15] +
              m[4]  * m[3] * m[13] +
              m[12] * m[1] * m[7] -
              m[12] * m[3] * m[5];

    inv[14] = -m[0]  * m[5] * m[14] +
               m[0]  * m[6] * m[13] +
               m[4]  * m[1] * m[14] -
               m[4]  * m[2] * m[13] -
               m[12] * m[1] * m[6] +
               m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] +
              m[1] * m[7] * m[10] +
              m[5] * m[2] * m[11] -
              m[5] * m[3] * m[10] -
              m[9] * m[2] * m[7] +
              m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] -
             m[0] * m[7] * m[10] -
             m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] -
             m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] +
               m[0] * m[7] * m[9] +
               m[4] * m[1] * m[11] -
               m[4] * m[3] * m[9] -
               m[8] * m[1] * m[7] +
               m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] -
              m[0] * m[6] * m[9] -
              m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];

    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || det == 0.0f)
        return false;

    const float invDet = 1.0f / det;
    Mat4 cand{};
    for (int i = 0; i < 16; ++i)
        (&cand.m[0][0])[i] = inv[i] * invDet;

    if (!MatrixIsInitialized(cand))
        return false;
    inverse = cand;
    return true;
}

inline Mat4 StreamlineProjection(const sl::float4x4& m) {
    const Mat4 rows = MakeMat4(
        m[0].x, m[0].y, m[0].z, m[0].w,
        m[1].x, m[1].y, m[1].z, m[1].w,
        m[2].x, m[2].y, m[2].z, m[2].w,
        m[3].x, m[3].y, m[3].z, m[3].w);
    return Transpose(rows);
}

inline bool TryHandedness(float clipWFromViewZ, bool& isRightHanded) {
    if (!IsInitialized(clipWFromViewZ) || clipWFromViewZ == 0.0f)
        return false;
    isRightHanded = clipWFromViewZ < 0.0f;
    return true;
}

inline Mat4 PerspectiveFovRH(float fovY, float aspect, float zn, float zf) {
    const float yScale = 1.0f / std::tan(fovY * 0.5f);
    const float xScale = yScale / aspect;
    const float q = zf / (zn - zf);
    return MakeMat4(
        xScale, 0.0f,   0.0f,      0.0f,
        0.0f,   yScale, 0.0f,      0.0f,
        0.0f,   0.0f,   q,        -1.0f,
        0.0f,   0.0f,   q * zn,    0.0f);
}

inline Mat4 PerspectiveFovLH(float fovY, float aspect, float zn, float zf) {
    const float yScale = 1.0f / std::tan(fovY * 0.5f);
    const float xScale = yScale / aspect;
    const float q = zf / (zf - zn);
    return MakeMat4(
        xScale, 0.0f,   0.0f,       0.0f,
        0.0f,   yScale, 0.0f,       0.0f,
        0.0f,   0.0f,   q,          1.0f,
        0.0f,   0.0f,  -q * zn,     0.0f);
}

inline bool TryScalarProjection(const sl::Constants& cam, bool depthInverted, Mat4& projection) {
    bool isRightHanded = false;
    if (!IsInitialized(cam.cameraFOV) || !IsInitialized(cam.cameraAspectRatio) ||
        !IsInitialized(cam.cameraNear) || !IsInitialized(cam.cameraFar) ||
        cam.orthographicProjection == sl::Boolean::eTrue ||
        !TryHandedness(cam.cameraViewToClip[2].w, isRightHanded))
        return false;

    const float fov = cam.cameraFOV < 4.0f ? cam.cameraFOV : cam.cameraFOV * (kPi / 180.0f);
    const float nearPlane = cam.cameraNear;
    const float farPlane = cam.cameraFar;
    if (!(fov > 0.0f && fov < kPi) || cam.cameraAspectRatio <= 0.0f ||
        nearPlane <= 0.0f || (farPlane != 0.0f && farPlane <= nearPlane))
        return false;

    Mat4 rows{};
    if (farPlane == 0.0f) {
        const float yScale = 1.0f / std::tan(fov * 0.5f);
        const float xScale = yScale / cam.cameraAspectRatio;
        const float W = isRightHanded ? -1.0f : 1.0f;
        const float A = depthInverted ? 0.0f : W;
        const float B = depthInverted ? nearPlane : -nearPlane;
        rows = MakeMat4(
            xScale, 0.0f,   0.0f, 0.0f,
            0.0f,   yScale, 0.0f, 0.0f,
            0.0f,   0.0f,   A,    W,
            0.0f,   0.0f,   B,    0.0f);
    } else {
        const float matrixNear = depthInverted ? farPlane : nearPlane;
        const float matrixFar = depthInverted ? nearPlane : farPlane;
        rows = isRightHanded
            ? PerspectiveFovRH(fov, cam.cameraAspectRatio, matrixNear, matrixFar)
            : PerspectiveFovLH(fov, cam.cameraAspectRatio, matrixNear, matrixFar);
    }

    const Mat4 cand = Transpose(rows);
    Mat4 inv{};
    if (!TryInvert(cand, inv))
        return false;
    projection = cand;
    return true;
}

inline bool VectorIsInitialized(const sl::float3& v) {
    return IsInitialized(v.x) && IsInitialized(v.y) && IsInitialized(v.z);
}

inline bool TryBasisView(const sl::Constants& cam, bool isRightHanded, Mat4& view) {
    if (!VectorIsInitialized(cam.cameraRight) || !VectorIsInitialized(cam.cameraUp) ||
        !VectorIsInitialized(cam.cameraFwd) || !VectorIsInitialized(cam.cameraPos))
        return false;

    const float forwardSign = isRightHanded ? -1.0f : 1.0f;
    const Mat4 cand = MakeMat4(
        cam.cameraRight.x, cam.cameraUp.x, forwardSign * cam.cameraFwd.x, cam.cameraPos.x,
        cam.cameraRight.y, cam.cameraUp.y, forwardSign * cam.cameraFwd.y, cam.cameraPos.y,
        cam.cameraRight.z, cam.cameraUp.z, forwardSign * cam.cameraFwd.z, cam.cameraPos.z,
        0.0f,              0.0f,           0.0f,                          1.0f);
    Mat4 candView{};
    if (!TryInvert(cand, candView))
        return false;
    view = candView;
    return true;
}

inline bool ReadNgxMatrix(const NVSDK_NGX_Parameter& params, const char* key, float (&out)[16]) {
    float* matrix = nullptr;
    if (params.Get(key, reinterpret_cast<void**>(&matrix)) != NVSDK_NGX_Result_Success || matrix == nullptr)
        return false;
    bool nonZero = false;
    for (int i = 0; i < 16; ++i) {
        if (!IsInitialized(matrix[i]))
            return false;
        nonZero = nonZero || matrix[i] != 0.0f;
    }
    if (!nonZero)
        return false;
    std::memcpy(out, matrix, sizeof(out));
    return true;
}

inline void ResolveCameraMatrices(const NVSDK_NGX_Parameter& params, bool depthInverted, float (&w2v)[16], float (&v2c)[16]) {
    const bool hasW2V = ReadNgxMatrix(params, NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, w2v);
    const bool hasV2C = ReadNgxMatrix(params, NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, v2c);

    Mat4 ngxView{};
    Mat4 ngxProj{};
    Mat4 inv{};
    if (hasW2V)
        ngxView = Transpose(LoadFloat4x4(w2v));
    if (hasV2C)
        ngxProj = Transpose(LoadFloat4x4(v2c));

    const bool validW2V = hasW2V && TryInvert(ngxView, inv);
    const bool validV2C = hasV2C && TryInvert(ngxProj, inv);
    if (validW2V && validV2C)
        return;

    SlCameraTracker::Instance().TryHookLoadedModule();
    sl::Constants slSnap{};
    if (!SlCameraTracker::Instance().GetSnapshot(slSnap))
        return;

    Mat4 resolvedProj{};
    bool hasProj = false;
    if (validV2C) {
        resolvedProj = ngxProj;
        hasProj = true;
    } else {
        const Mat4 fullProj = StreamlineProjection(slSnap.cameraViewToClip);
        if (TryInvert(fullProj, inv)) {
            resolvedProj = fullProj;
            hasProj = true;
        } else if (TryScalarProjection(slSnap, depthInverted, resolvedProj)) {
            hasProj = true;
        }
    }

    if (!validV2C && hasProj) {
        const Mat4 rows = Transpose(resolvedProj);
        StoreFloat4x4(v2c, rows);
    }

    if (!validW2V && hasProj) {
        bool isRightHanded = false;
        Mat4 resolvedView{};
        if (TryHandedness(resolvedProj.m[3][2], isRightHanded) && TryBasisView(slSnap, isRightHanded, resolvedView)) {
            const Mat4 rows = Transpose(resolvedView);
            StoreFloat4x4(w2v, rows);
        }
    }
}

} // namespace camera
} // namespace openrr
