#pragma once
#include <cstdint>

namespace SkeletonPoseLogger
{
    void Initialize();
}

// ============================================================================
// Renderer-side types for blend matrix capture
// ============================================================================

// Forward declarations
struct IShaderParameterProvider;
struct CBufferRenderResource;

// ISceneObjectPrivateData — base class for per-object renderer private data
struct ISceneObjectPrivateData
{
    void* __vftable;
};

// CSceneObjectHandle2<T> — lightweight handle (proxy pointer)
struct CSceneObjectProxyBase;
template<typename T>
struct CSceneObjectHandle2
{
    CSceneObjectProxyBase* m_proxy;
    uint32_t m_index;
    uint32_t m_generation;
};

struct CSceneSkeleton; // forward decl

// IShaderParameter — base (4 bytes, padded to 8 in derived types)
struct IShaderParameter
{
    uint16_t m_preInitDirtyBits;  // +0x00
    uint16_t m_dirtyBitsOffset;   // +0x02
};
static_assert(sizeof(IShaderParameter) == 0x04, "IShaderParameter size mismatch");

// ndVector<float> inline for this header (same layout as ndVector<T> in SkeletonPoseLogger.cpp)
struct ndVectorFloat
{
    int64_t props;  // sign bit = inline; (props >> 32) & 0x7FFFFFFF = count
    float*  data;   // heap ptr if not inline

    uint32_t size() const { return (uint32_t)(((uint64_t)props >> 32) & 0x7FFFFFFF); }
    float* ptr() { return (props < 0) ? reinterpret_cast<float*>(&data) : data; }
    const float* ptr() const { return (props < 0) ? reinterpret_cast<const float*>(&data) : data; }
};

// CShaderParameterRawVector<float,0,4,1534> — holds the actual matrix data
// sizeof = 0x28
struct CShaderParameterRawVector_float
{
    IShaderParameter _base;              // +0x00
    uint32_t _pad04;                     // +0x04  padding to align ndVector at +0x08
    ndVectorFloat m_data;                // +0x08  the actual float array
    void* m_ptr;                         // +0x18  cached pointer
    uint16_t m_size;                     // +0x20  element count
    char _pad22[6];                      // +0x22
};
static_assert(sizeof(CShaderParameterRawVector_float) == 0x28, "CShaderParameterRawVector size mismatch");

// CShaderParameterVector<G4::VectorSIMD4f,1534> is the same as the raw vector
typedef CShaderParameterRawVector_float CShaderParameterVector_VectorSIMD4f;

// CShaderParameter<Device3D::CBuffer*>
struct CShaderParameter_CBuffer
{
    uint64_t IShaderParameter; // +0x00
    void*    m_buffer;         // +0x08
};

// CBlendMatricesParameterProvider — holds final blend matrices for GPU upload
// sizeof = 0x90, align 16
struct CBlendMatricesParameterProvider
{
    char _base[0x20];                                      // +0x00  CShaderParameterProviderBase
    CShaderParameter_CBuffer m_skinningConfig;             // +0x20
    CShaderParameterRawVector_float m_matrices;            // +0x30  current frame blend matrices
    CShaderParameterRawVector_float m_prevMatrices;        // +0x58  previous frame blend matrices
    bool m_dirty;                                          // +0x80
    char _pad81[0x0F];                                     // +0x81

    // Helper: get the current blend matrix float array (post-transpose, float4x3 layout)
    // Returns numBones * 12 floats (3 float4 registers per bone)
    float* getMatrixData()
    {
        return m_matrices.m_data.ptr();
    }

    uint32_t getMatrixFloatCount()
    {
        return m_matrices.m_data.size();
    }
};
static_assert(sizeof(CBlendMatricesParameterProvider) == 0x90, "CBlendMatricesParameterProvider size mismatch");

// CRendererHelpers — partial, only need pointer access
struct CRendererHelpers;

// CSceneSkeletonPrivateData — renderer-side skeleton data
// sizeof = 0x50
struct ndVector_Matrix44;
struct CSceneSkeletonPrivateData : ISceneObjectPrivateData
{
    CSceneObjectHandle2<CSceneSkeleton> m_skeletonHandle;  // +0x08
    CBlendMatricesParameterProvider* m_parameterProvider;   // +0x18
    char m_nextPrevBones[0x10];                            // +0x20  ndVector<Matrix44_tpl<float>>
    char m_prevBones[0x10];                                // +0x30  ndVector<Matrix44_tpl<float>>
    uint32_t m_prevBonesSyncCounter;                       // +0x40
    uint32_t m_nextPrevBonesSyncCounter;                   // +0x44
    CRendererHelpers* m_rendererHelpers;                   // +0x48
};
static_assert(sizeof(CSceneSkeletonPrivateData) == 0x50, "CSceneSkeletonPrivateData size mismatch");
