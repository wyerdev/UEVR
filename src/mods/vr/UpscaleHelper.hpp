#pragma once
#include <safetyhook.hpp>

// COM-like Release macro
#ifndef SAFE_RELEASE
	#define SAFE_RELEASE(a) if (a) { a->Release(); a = NULL; }
#endif

#define NVSDK_NGX_Result_Success 0x1
typedef int NVSDK_NGX_Result;
typedef enum NVSDK_NGX_Feature {
    NVSDK_NGX_Feature_SuperSampling = 1,
    NVSDK_NGX_Feature_RayReconstruction = 13,
} NVSDK_NGX_Feature;
typedef struct NVSDK_NGX_Handle {
    unsigned int Id;
} NVSDK_NGX_Handle;
typedef struct NVSDK_NGX_Parameter {
    virtual void Set(const char* InName, unsigned long long InValue) = 0;
    virtual void Set(const char* InName, float InValue) = 0;
    virtual void Set(const char* InName, double InValue) = 0;
    virtual void Set(const char* InName, unsigned int InValue) = 0;
    virtual void Set(const char* InName, int InValue) = 0;
    virtual void Set(const char* InName, ID3D11Resource* InValue) = 0;
    virtual void Set(const char* InName, ID3D12Resource* InValue) = 0;
    virtual void Set(const char* InName, void* InValue) = 0;

    virtual NVSDK_NGX_Result Get(const char* InName, unsigned long long* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, float* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, double* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, unsigned int* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, int* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, ID3D11Resource** OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, ID3D12Resource** OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, void** OutValue) const = 0;

    virtual void Reset() = 0;
} NVSDK_NGX_Parameter;

#define NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags "DLSS.Feature.Create.Flags"
#define NVSDK_NGX_Parameter_OutWidth "OutWidth"
#define NVSDK_NGX_Parameter_OutHeight "OutHeight"
using NVSDK_NGX_D3D12_CreateFeature_t = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID, const NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle);
static NVSDK_NGX_D3D12_CreateFeature_t o_NVSDK_NGX_D3D12_CreateFeature = nullptr;
static SafetyHookInline NVSDK_NGX_D3D12_CreateFeature_Hook{};
NVSDK_NGX_Result hk_NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID, NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle);

using NVSDK_NGX_D3D12_ReleaseFeature_t = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle* InHandle);
static NVSDK_NGX_D3D12_ReleaseFeature_t o_NVSDK_NGX_D3D12_ReleaseFeature = nullptr;
static SafetyHookInline NVSDK_NGX_D3D12_ReleaseFeature_Hook{};
NVSDK_NGX_Result hk_NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* InHandle);

using NVSDK_NGX_D3D12_EvaluateFeature_t = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList* InCmdList, const NVSDK_NGX_Handle* InFeatureHandle, NVSDK_NGX_Parameter* InParameters, void* InCallback);
static NVSDK_NGX_D3D12_EvaluateFeature_t o_NVSDK_NGX_D3D12_EvaluateFeature = nullptr;
static SafetyHookInline NVSDK_NGX_D3D12_EvaluateFeature_Hook{};
NVSDK_NGX_Result hk_NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* InCmdList, const NVSDK_NGX_Handle* InFeatureHandle, NVSDK_NGX_Parameter* InParameters, void* InCallback);


#define NVSDK_NGX_SUCCEED(value) (((value) & 0xFFF00000) != 0xBAD00000)
#define NVSDK_NGX_FAILED(value) (((value) & 0xFFF00000) == 0xBAD00000)
#define NVSDK_NGX_Parameter_Color "Color"
#define NVSDK_NGX_Parameter_Depth "Depth"
#define NVSDK_NGX_Parameter_MotionVectors "MotionVectors"
#define NVSDK_NGX_Parameter_Output "Output"
#define NVSDK_NGX_Parameter_MV_Scale_X "MV.Scale.X"
#define NVSDK_NGX_Parameter_MV_Scale_Y "MV.Scale.Y"
#define NVSDK_NGX_Parameter_Jitter_Offset_X "Jitter.Offset.X"
#define NVSDK_NGX_Parameter_Jitter_Offset_Y "Jitter.Offset.Y"
#define NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width "DLSS.Render.Subrect.Dimensions.Width"
#define NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height "DLSS.Render.Subrect.Dimensions.Height"



typedef void* ffxContext;
typedef uint32_t ffxReturnCode_t;
typedef uint64_t ffxStructType_t;
typedef struct ffxApiHeader {
    ffxStructType_t type;
    struct ffxApiHeader* pNext;
} ffxApiHeader;
typedef ffxApiHeader ffxCreateContextDescHeader;
typedef ffxApiHeader ffxDispatchDescHeader;
struct FfxApiResource {
    void* resource;
    uint32_t description[8];
    uint32_t state;
};
struct FfxApiDimensions2D {
    uint32_t width;
    uint32_t height;
};
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE 0x00010000u
struct ffxCreateContextDescUpscale {
    ffxCreateContextDescHeader header;
    uint32_t flags;
    struct FfxApiDimensions2D maxRenderSize;
    struct FfxApiDimensions2D maxUpscaleSize;
};

#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE 0x00010001u
struct ffxDispatchDescUpscale {
    ffxDispatchDescHeader header;
    void* commandList;
    struct FfxApiResource color;
    struct FfxApiResource depth;
    struct FfxApiResource motionVectors;
    struct FfxApiResource exposure;
    struct FfxApiResource reactive;
    struct FfxApiResource transparencyAndComposition;
    struct FfxApiResource output;
    float jitterOffset[2];
    float motionVectorScale[2];
    float renderSize[2];
    float upscaleSize[2];
    bool enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    uint32_t flags;
};

using ffxCreateContext_t = ffxReturnCode_t (*)(ffxContext* context, ffxCreateContextDescHeader* desc, const void** memCb);
static ffxCreateContext_t o_ffxCreateContext = nullptr;
static SafetyHookInline ffxCreateContext_Hook{};
ffxReturnCode_t hk_ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const void** memCb);

using ffxDestroyContext_t = ffxReturnCode_t (*)(ffxContext* context, const void** memCb);
static ffxDestroyContext_t o_ffxDestroyContext = nullptr;
static SafetyHookInline ffxDestroyContext_Hook{};
ffxReturnCode_t hk_ffxDestroyContext(ffxContext* context, const void** memCb);

using ffxDispatch_t = ffxReturnCode_t (*)(ffxContext* context, const ffxDispatchDescHeader* desc);
static ffxDispatch_t o_ffxDispatch = nullptr;
static SafetyHookInline ffxDispatch_Hook{};
ffxReturnCode_t hk_ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc);


typedef enum _xess_result_t {
    XESS_RESULT_WARNING_NONEXISTING_FOLDER = 1,
    XESS_RESULT_WARNING_OLD_DRIVER = 2,
    XESS_RESULT_SUCCESS = 0,
    XESS_RESULT_ERROR_UNSUPPORTED_DEVICE = -1,
    XESS_RESULT_ERROR_UNSUPPORTED_DRIVER = -2,
    XESS_RESULT_ERROR_UNINITIALIZED = -3,
    XESS_RESULT_ERROR_INVALID_ARGUMENT = -4,
    XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY = -5,
    XESS_RESULT_ERROR_DEVICE = -6,
    XESS_RESULT_ERROR_NOT_IMPLEMENTED = -7,
    XESS_RESULT_ERROR_INVALID_CONTEXT = -8,
    XESS_RESULT_ERROR_OPERATION_IN_PROGRESS = -9,
    XESS_RESULT_ERROR_UNSUPPORTED = -10,
    XESS_RESULT_ERROR_CANT_LOAD_LIBRARY = -11,
    XESS_RESULT_ERROR_WRONG_CALL_ORDER = -12,
    XESS_RESULT_ERROR_UNKNOWN = -1000,
} xess_result_t;
typedef enum _xess_quality_settings_t {
    XESS_QUALITY_SETTING_ULTRA_PERFORMANCE = 100,
    XESS_QUALITY_SETTING_PERFORMANCE = 101,
    XESS_QUALITY_SETTING_BALANCED = 102,
    XESS_QUALITY_SETTING_QUALITY = 103,
    XESS_QUALITY_SETTING_ULTRA_QUALITY = 104,
    XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS = 105,
    XESS_QUALITY_SETTING_AA = 106,
} xess_quality_settings_t;
typedef struct _xess_context_handle_t* xess_context_handle_t;
typedef struct _xess_2d_t {
    uint32_t x;
    uint32_t y;
} xess_2d_t;
typedef xess_2d_t xess_coord_t;

typedef enum _xess_init_flags_t {
    XESS_INIT_FLAG_NONE = 0,
    XESS_INIT_FLAG_HIGH_RES_MV = 1 << 0,
    XESS_INIT_FLAG_INVERTED_DEPTH = 1 << 1,
    XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE = 1 << 2,
    XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK = 1 << 3,
    XESS_INIT_FLAG_USE_NDC_VELOCITY = 1 << 4,
    XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP = 1 << 5,
    XESS_INIT_FLAG_LDR_INPUT_COLOR = 1 << 6,
    XESS_INIT_FLAG_JITTERED_MV = 1 << 7,
    XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE = 1 << 8
} xess_init_flags_t;
typedef struct _xess_d3d12_init_params_t {
    xess_2d_t outputResolution;
    xess_quality_settings_t qualitySetting;
    uint32_t initFlags;
    uint32_t creationNodeMask;
    uint32_t visibleNodeMask;
    ID3D12Heap* pTempBufferHeap;
    uint64_t bufferHeapOffset;
    ID3D12Heap* pTempTextureHeap;
    uint64_t textureHeapOffset;
    ID3D12PipelineLibrary* pPipelineLibrary;
} xess_d3d12_init_params_t;
typedef struct _xess_d3d12_execute_params_t {
    ID3D12Resource* pColorTexture;
    ID3D12Resource* pVelocityTexture;
    ID3D12Resource* pDepthTexture;
    ID3D12Resource* pExposureScaleTexture;
    ID3D12Resource* pResponsivePixelMaskTexture;
    ID3D12Resource* pOutputTexture;
    float jitterOffsetX;
    float jitterOffsetY;
    float exposureScale;
    uint32_t resetHistory;
    uint32_t inputWidth;
    uint32_t inputHeight;
    xess_coord_t inputColorBase;
    xess_coord_t inputMotionVectorBase;
    xess_coord_t inputDepthBase;
    xess_coord_t inputResponsiveMaskBase;
    xess_coord_t reserved0;
    xess_coord_t outputColorBase;
    ID3D12DescriptorHeap* pDescriptorHeap;
    uint32_t descriptorHeapOffset;
} xess_d3d12_execute_params_t;

using xessD3D12CreateContext_t = xess_result_t (*)(ID3D12Device* pDevice, xess_context_handle_t* phContext);
using xessDestroyContext_t = xess_result_t (*)(xess_context_handle_t hContext);
using xessD3D12Init_t = xess_result_t (*)(xess_context_handle_t hContext, const xess_d3d12_init_params_t* pInitParams);
using xessD3D12Execute_t = xess_result_t (*)(xess_context_handle_t hContext, ID3D12GraphicsCommandList* pCommandList, const xess_d3d12_execute_params_t* pExecParams);

static xessD3D12CreateContext_t o_xessD3D12CreateContext = nullptr;
static SafetyHookInline xessD3D12CreateContext_Hook{};
xess_result_t hk_xessD3D12CreateContext(ID3D12Device* pDevice, xess_context_handle_t* phContext);

static xessDestroyContext_t o_xessDestroyContext = nullptr;
static SafetyHookInline xessDestroyContext_Hook{};
xess_result_t hk_xessDestroyContext(xess_context_handle_t* phContext);

static xessD3D12Init_t o_xessD3D12Init = nullptr;
static SafetyHookInline xessD3D12Init_Hook{};
xess_result_t hk_xessD3D12Init(xess_context_handle_t hContext, const xess_d3d12_init_params_t* pInitParams);

static xessD3D12Execute_t o_xessD3D12Execute = nullptr;
static SafetyHookInline xessD3D12Execute_Hook{};
xess_result_t hk_xessD3D12Execute(xess_context_handle_t hContext, ID3D12GraphicsCommandList* pCommandList, const xess_d3d12_execute_params_t* pExecParams);