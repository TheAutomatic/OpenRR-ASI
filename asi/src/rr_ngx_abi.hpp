// SPDX-License-Identifier: MIT
// Adapted from DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
// Transcribed from NVIDIA NGX SDK headers for ABI interoperability without SDK header dependencies.
#pragma once

#include "rr_ngx_slots.hpp"

#include <cstddef>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGIAdapter;

using RrVkInstance = void*;
using RrVkPhysicalDevice = void*;
using RrVkDevice = void*;
using RrVkCommandBuffer = void*;

typedef enum NVSDK_NGX_Result
{
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_Fail = 0xBAD00000,
    NVSDK_NGX_Result_FAIL_FeatureNotSupported = NVSDK_NGX_Result_Fail | 1,
    NVSDK_NGX_Result_FAIL_PlatformError = NVSDK_NGX_Result_Fail | 2,
    NVSDK_NGX_Result_FAIL_FeatureAlreadyExists = NVSDK_NGX_Result_Fail | 3,
    NVSDK_NGX_Result_FAIL_FeatureNotFound = NVSDK_NGX_Result_Fail | 4,
    NVSDK_NGX_Result_FAIL_InvalidParameter = NVSDK_NGX_Result_Fail | 5,
    NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall = NVSDK_NGX_Result_Fail | 6,
    NVSDK_NGX_Result_FAIL_NotInitialized = NVSDK_NGX_Result_Fail | 7,
    NVSDK_NGX_Result_FAIL_UnsupportedInputFormat = NVSDK_NGX_Result_Fail | 8,
    NVSDK_NGX_Result_FAIL_RWFlagMissing = NVSDK_NGX_Result_Fail | 9,
    NVSDK_NGX_Result_FAIL_MissingInput = NVSDK_NGX_Result_Fail | 10,
    NVSDK_NGX_Result_FAIL_UnableToInitializeFeature = NVSDK_NGX_Result_Fail | 11,
    NVSDK_NGX_Result_FAIL_OutOfDate = NVSDK_NGX_Result_Fail | 12,
    NVSDK_NGX_Result_FAIL_OutOfGPUMemory = NVSDK_NGX_Result_Fail | 13,
    NVSDK_NGX_Result_FAIL_UnsupportedFormat = NVSDK_NGX_Result_Fail | 14,
    NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath = NVSDK_NGX_Result_Fail | 15,
    NVSDK_NGX_Result_FAIL_UnsupportedParameter = NVSDK_NGX_Result_Fail | 16,
    NVSDK_NGX_Result_FAIL_Denied = NVSDK_NGX_Result_Fail | 17,
    NVSDK_NGX_Result_FAIL_NotImplemented = NVSDK_NGX_Result_Fail | 18,
} NVSDK_NGX_Result;

#define NVSDK_NGX_SUCCEED(value) (((value) & 0xFFF00000) != NVSDK_NGX_Result_Fail)
#define NVSDK_NGX_FAILED(value) (((value) & 0xFFF00000) == NVSDK_NGX_Result_Fail)

typedef enum NVSDK_NGX_Feature
{
    NVSDK_NGX_Feature_Reserved0 = 0,
    NVSDK_NGX_Feature_SuperSampling = 1,
    NVSDK_NGX_Feature_FrameGeneration = 11,
    NVSDK_NGX_Feature_RayReconstruction = 13,
    NVSDK_NGX_Feature_NeuralRendering = 18,
} NVSDK_NGX_Feature;

typedef enum NVSDK_NGX_Version
{
    NVSDK_NGX_Version_API = 0x0000015,
} NVSDK_NGX_Version;

typedef enum NVSDK_NGX_EngineType
{
    NVSDK_NGX_ENGINE_TYPE_CUSTOM = 0,
    NVSDK_NGX_ENGINE_TYPE_UNREAL = 1,
    NVSDK_NGX_ENGINE_TYPE_UNITY = 2,
    NVSDK_NGX_ENGINE_TYPE_OMNIVERSE = 3,
    NVSDK_NGX_ENGINE_COUNT = 4,
} NVSDK_NGX_EngineType;

typedef enum NVSDK_NGX_PerfQuality_Value
{
    NVSDK_NGX_PerfQuality_Value_MaxPerf = 0,
    NVSDK_NGX_PerfQuality_Value_Balanced = 1,
    NVSDK_NGX_PerfQuality_Value_MaxQuality = 2,
    NVSDK_NGX_PerfQuality_Value_UltraPerformance = 3,
    NVSDK_NGX_PerfQuality_Value_UltraQuality = 4,
    NVSDK_NGX_PerfQuality_Value_DLAA = 5,
} NVSDK_NGX_PerfQuality_Value;

typedef enum NVSDK_NGX_DLSS_Mode
{
    NVSDK_NGX_DLSS_Mode_Off = 0,
    NVSDK_NGX_DLSS_Mode_DLSS_DLISP = 1,
    NVSDK_NGX_DLSS_Mode_DLISP_Only = 2,
    NVSDK_NGX_DLSS_Mode_DLSS = 3,
} NVSDK_NGX_DLSS_Mode;

typedef enum NVSDK_NGX_DLSS_Feature_Flags
{
    NVSDK_NGX_DLSS_Feature_Flags_IsInvalid = 1 << 31,
    NVSDK_NGX_DLSS_Feature_Flags_None = 0,
    NVSDK_NGX_DLSS_Feature_Flags_IsHDR = 1 << 0,
    NVSDK_NGX_DLSS_Feature_Flags_MVLowRes = 1 << 1,
    NVSDK_NGX_DLSS_Feature_Flags_MVJittered = 1 << 2,
    NVSDK_NGX_DLSS_Feature_Flags_DepthInverted = 1 << 3,
    NVSDK_NGX_DLSS_Feature_Flags_Reserved_0 = 1 << 4,
    NVSDK_NGX_DLSS_Feature_Flags_DoSharpening = 1 << 5,
    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure = 1 << 6,
    NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling = 1 << 7,
} NVSDK_NGX_DLSS_Feature_Flags;

typedef enum NVSDK_NGX_DLSS_Denoise_Mode
{
    NVSDK_NGX_DLSS_Denoise_Mode_Off = 0,
    NVSDK_NGX_DLSS_Denoise_Mode_DLUnified = 1,
} NVSDK_NGX_DLSS_Denoise_Mode;

typedef enum NVSDK_NGX_DLSS_Roughness_Mode
{
    NVSDK_NGX_DLSS_Roughness_Mode_Unpacked = 0,
    NVSDK_NGX_DLSS_Roughness_Mode_Packed = 1,
} NVSDK_NGX_DLSS_Roughness_Mode;

typedef enum NVSDK_NGX_DLSS_Depth_Type
{
    NVSDK_NGX_DLSS_Depth_Type_Linear = 0,
    NVSDK_NGX_DLSS_Depth_Type_HW = 1,
} NVSDK_NGX_DLSS_Depth_Type;

typedef enum NVSDK_NGX_Feature_Support_Result
{
    NVSDK_NGX_FeatureSupportResult_Supported = 0,
    NVSDK_NGX_FeatureSupportResult_CheckNotPresent = 1,
    NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported = 2,
    NVSDK_NGX_FeatureSupportResult_AdapterUnsupported = 4,
    NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported = 8,
    NVSDK_NGX_FeatureSupportResult_NotImplemented = 16,
} NVSDK_NGX_Feature_Support_Result;

typedef enum NVSDK_NGX_Application_Identifier_Type
{
    NVSDK_NGX_Application_Identifier_Type_Application_Id = 0,
    NVSDK_NGX_Application_Identifier_Type_Project_Id = 1,
} NVSDK_NGX_Application_Identifier_Type;

typedef struct NVSDK_NGX_ProjectIdDescription
{
    const char* ProjectId;
    NVSDK_NGX_EngineType EngineType;
    const char* EngineVersion;
} NVSDK_NGX_ProjectIdDescription;

typedef struct NVSDK_NGX_Application_Identifier
{
    NVSDK_NGX_Application_Identifier_Type IdentifierType;
    union
    {
        NVSDK_NGX_ProjectIdDescription ProjectDesc;
        unsigned long long ApplicationId;
    } v;
} NVSDK_NGX_Application_Identifier;

typedef struct NVSDK_NGX_PathListInfo
{
    wchar_t const* const* Path;
    unsigned int Length;
} NVSDK_NGX_PathListInfo;

typedef struct NVSDK_NGX_LoggingInfo
{
    void* LoggingCallback;
    int MinimumLoggingLevel;
    bool DisableOtherLoggingSinks;
} NVSDK_NGX_LoggingInfo;

typedef struct NVSDK_NGX_FeatureCommonInfo
{
    NVSDK_NGX_PathListInfo PathListInfo;
    void* InternalData;
    NVSDK_NGX_LoggingInfo LoggingInfo;
} NVSDK_NGX_FeatureCommonInfo;

typedef struct NVSDK_NGX_FeatureDiscoveryInfo
{
    NVSDK_NGX_Version SDKVersion;
    NVSDK_NGX_Feature FeatureID;
    NVSDK_NGX_Application_Identifier Identifier;
    const wchar_t* ApplicationDataPath;
    const NVSDK_NGX_FeatureCommonInfo* FeatureInfo;
} NVSDK_NGX_FeatureDiscoveryInfo;

typedef struct NVSDK_NGX_FeatureRequirement
{
    NVSDK_NGX_Feature_Support_Result FeatureSupported;
    unsigned int MinHWArchitecture;
    char MinOSVersion[255];
} NVSDK_NGX_FeatureRequirement;

typedef struct NVSDK_NGX_Handle
{
    unsigned int Id;
} NVSDK_NGX_Handle;

struct NVSDK_NGX_Parameter;

struct NVSDK_NGX_ParameterVtbl
{
    void (RR_NGX_MSABI* SetVoidPtr)(NVSDK_NGX_Parameter*, const char*, void*);
    void (RR_NGX_MSABI* SetD3D12Resource)(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*);
    void (RR_NGX_MSABI* SetD3D11Resource)(NVSDK_NGX_Parameter*, const char*, ID3D11Resource*);
    void (RR_NGX_MSABI* SetInt)(NVSDK_NGX_Parameter*, const char*, int);
    void (RR_NGX_MSABI* SetUInt)(NVSDK_NGX_Parameter*, const char*, unsigned int);
    void (RR_NGX_MSABI* SetDouble)(NVSDK_NGX_Parameter*, const char*, double);
    void (RR_NGX_MSABI* SetFloat)(NVSDK_NGX_Parameter*, const char*, float);
    void (RR_NGX_MSABI* SetULL)(NVSDK_NGX_Parameter*, const char*, unsigned long long);

    NVSDK_NGX_Result (RR_NGX_MSABI* GetVoidPtr)(const NVSDK_NGX_Parameter*, const char*, void**);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetD3D12Resource)(const NVSDK_NGX_Parameter*, const char*, ID3D12Resource**);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetD3D11Resource)(const NVSDK_NGX_Parameter*, const char*, ID3D11Resource**);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetInt)(const NVSDK_NGX_Parameter*, const char*, int*);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetUInt)(const NVSDK_NGX_Parameter*, const char*, unsigned int*);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetDouble)(const NVSDK_NGX_Parameter*, const char*, double*);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetFloat)(const NVSDK_NGX_Parameter*, const char*, float*);
    NVSDK_NGX_Result (RR_NGX_MSABI* GetULL)(const NVSDK_NGX_Parameter*, const char*, unsigned long long*);

    void (RR_NGX_MSABI* Reset)(NVSDK_NGX_Parameter*);
};

struct NVSDK_NGX_Parameter
{
    const NVSDK_NGX_ParameterVtbl* vtbl;

    void Set(const char* n, void* v)                { vtbl->SetVoidPtr(this, n, v); }
    void Set(const char* n, ID3D12Resource* v)      { vtbl->SetD3D12Resource(this, n, v); }
    void Set(const char* n, ID3D11Resource* v)      { vtbl->SetD3D11Resource(this, n, v); }
    void Set(const char* n, int v)                  { vtbl->SetInt(this, n, v); }
    void Set(const char* n, unsigned int v)         { vtbl->SetUInt(this, n, v); }
    void Set(const char* n, double v)               { vtbl->SetDouble(this, n, v); }
    void Set(const char* n, float v)                { vtbl->SetFloat(this, n, v); }
    void Set(const char* n, unsigned long long v)   { vtbl->SetULL(this, n, v); }

    NVSDK_NGX_Result Get(const char* n, void** v) const               { return vtbl->GetVoidPtr(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** v) const     { return vtbl->GetD3D12Resource(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** v) const     { return vtbl->GetD3D11Resource(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, int* v) const                 { return vtbl->GetInt(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* v) const        { return vtbl->GetUInt(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, double* v) const              { return vtbl->GetDouble(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, float* v) const               { return vtbl->GetFloat(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned long long* v) const  { return vtbl->GetULL(this, n, v); }

    void Reset() { vtbl->Reset(this); }
};

static_assert(sizeof(NVSDK_NGX_Parameter) == sizeof(void*),
              "the parameter block is a vtable pointer and nothing else");
static_assert(sizeof(NVSDK_NGX_ParameterVtbl) == 17 * sizeof(void*),
              "17 slots: eight setters, eight getters, Reset");

typedef enum NVSDK_NGX_Resource_VK_Type
{
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW = 0,
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER = 1,
} NVSDK_NGX_Resource_VK_Type;

typedef struct NVSDK_NGX_ImageViewInfo_VK
{
    void* ImageView;
    void* Image;
    struct
    {
        unsigned int aspectMask;
        unsigned int baseMipLevel;
        unsigned int levelCount;
        unsigned int baseArrayLayer;
        unsigned int layerCount;
    } SubresourceRange;
    unsigned int Format;
    unsigned int Width;
    unsigned int Height;
} NVSDK_NGX_ImageViewInfo_VK;

typedef struct NVSDK_NGX_BufferInfo_VK
{
    void* Buffer;
    unsigned int SizeInBytes;
} NVSDK_NGX_BufferInfo_VK;

typedef struct NVSDK_NGX_Resource_VK
{
    union
    {
        NVSDK_NGX_ImageViewInfo_VK ImageViewInfo;
        NVSDK_NGX_BufferInfo_VK BufferInfo;
    } Resource;
    NVSDK_NGX_Resource_VK_Type Type;
    bool ReadWrite;
} NVSDK_NGX_Resource_VK;

// Common NGX / DLSSD parameter keys
#define NVSDK_NGX_Parameter_Width "Width"
#define NVSDK_NGX_Parameter_Height "Height"
#define NVSDK_NGX_Parameter_OutWidth "OutWidth"
#define NVSDK_NGX_Parameter_OutHeight "OutHeight"
#define NVSDK_NGX_Parameter_Scale "Scale"
#define NVSDK_NGX_Parameter_SuperSampling_ScaleFactor "SuperSampling.ScaleFactor"
#define NVSDK_NGX_Parameter_PerfQualityValue "PerfQualityValue"
#define NVSDK_NGX_Parameter_SizeInBytes "SizeInBytes"
#define NVSDK_NGX_Parameter_DLSSMode "DLSSMode"
#define NVSDK_NGX_Parameter_Reset "Reset"
#define NVSDK_NGX_Parameter_Color "Color"
#define NVSDK_NGX_Parameter_Output "Output"
#define NVSDK_NGX_Parameter_Depth "Depth"
#define NVSDK_NGX_Parameter_MotionVectors "MotionVectors"
#define NVSDK_NGX_Parameter_Jitter_Offset_X "Jitter.Offset.X"
#define NVSDK_NGX_Parameter_Jitter_Offset_Y "Jitter.Offset.Y"
#define NVSDK_NGX_Parameter_MV_Scale_X "MV.Scale.X"
#define NVSDK_NGX_Parameter_MV_Scale_Y "MV.Scale.Y"
#define NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags "DLSS.Feature.Create.Flags"
#define NVSDK_NGX_Parameter_GBuffer_Normals "GBuffer.Normals"
#define NVSDK_NGX_Parameter_GBuffer_Roughness "GBuffer.Roughness"
#define NVSDK_NGX_Parameter_GBuffer_SpecularMvec "GBuffer.SpecularMvec"
#define NVSDK_NGX_Parameter_DiffuseAlbedo "DLSS.Input.DiffuseAlbedo"
#define NVSDK_NGX_Parameter_SpecularAlbedo "DLSS.Input.SpecularAlbedo"
#define NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X "DLSS.Input.Color.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y "DLSS.Input.Color.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X "DLSS.Input.Depth.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y "DLSS.Input.Depth.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X "DLSS.Input.MV.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y "DLSS.Input.MV.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X "DLSS.Input.DiffuseAlbedo.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y "DLSS.Input.DiffuseAlbedo.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X "DLSS.Input.SpecularAlbedo.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y "DLSS.Input.SpecularAlbedo.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X "DLSS.Input.Normals.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y "DLSS.Input.Normals.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X "DLSS.Input.Roughness.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y "DLSS.Input.Roughness.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width "DLSS.Render.Subrect.Dimensions.Width"
#define NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height "DLSS.Render.Subrect.Dimensions.Height"
#define NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width "DLSS.Get.Dynamic.Max.Render.Width"
#define NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height "DLSS.Get.Dynamic.Max.Render.Height"
#define NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width "DLSS.Get.Dynamic.Min.Render.Width"
#define NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height "DLSS.Get.Dynamic.Min.Render.Height"
#define NVSDK_NGX_Parameter_DLSS_Denoise_Mode "DLSS.Denoise.Mode"
#define NVSDK_NGX_Parameter_DLSS_Roughness_Mode "DLSS.Roughness.Mode"
#define NVSDK_NGX_Parameter_Use_HW_Depth "DLSS.Use.HW.Depth"
#define NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX "WorldToViewMatrix"
#define NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX "ViewToClipMatrix"
#define NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance "DLSSD.SpecularHitDistance"
#define NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X "DLSSD.SpecularHitDistance.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y "DLSSD.SpecularHitDistance.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance "DLSSD.SpecularRayDirectionHitDistance"
#define NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_X "DLSSD.SpecularRayDirectionHitDistance.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_Y "DLSSD.SpecularRayDirectionHitDistance.Subrect.Base.Y"
#define NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles "DLSSD.ColorBeforeParticles"
#define NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency "DLSSD.ColorBeforeTransparency"
#define NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog "DLSSD.ColorBeforeFog"
#define NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide "DLSSD.ScreenSpaceSubsurfaceScatteringGuide"
#define NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide "DLSSD.DepthOfFieldGuide"
#define NVSDK_NGX_Parameter_SuperSamplingDenoising_Available "SuperSamplingDenoising.Available"
#define NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver "SuperSamplingDenoising.NeedsUpdatedDriver"
#define NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor "SuperSamplingDenoising.MinDriverVersionMajor"
#define NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor "SuperSamplingDenoising.MinDriverVersionMinor"
#define NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult "SuperSamplingDenoising.FeatureInitResult"
#define NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback "DLSSDOptimalSettingsCallback"
#define NVSDK_NGX_Parameter_DLSSDGetStatsCallback "DLSSDGetStatsCallback"

#define NVSDK_NGX_EParameter_Scale "#\x0f"
#define NVSDK_NGX_EParameter_OutWidth "#\x12"
#define NVSDK_NGX_EParameter_OutHeight "#\x13"
#define NVSDK_NGX_EParameter_SizeInBytes "#\x30"
#define NVSDK_NGX_EParameter_DLSSMode "#\x41"
