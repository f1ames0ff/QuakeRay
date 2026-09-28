// Copyright (c) 2026 QuakeRay contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "VertexCollector.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "Generated/ShaderCommonC.h"
#include "Matrix.h"

using namespace qray;

constexpr uint32_t INDEX_BUFFER_SIZE     = MAX_INDEXED_PRIMITIVE_COUNT * 3 * sizeof( uint32_t );
constexpr uint32_t TRANSFORM_BUFFER_SIZE =  MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT * sizeof( VkTransformMatrixKHR );

namespace
{
    uint32_t AlignUpBy3( uint32_t x )
    {
        return ( ( x + 2 ) / 3 ) * 3;
    }

    uint32_t GetMaterialsBlendFlags( const QrGeometryMaterialBlendType blendingTypes[],
                                     uint32_t                          count )
    {
        uint32_t r = 0;

        for( uint32_t i = 0; i < count; i++ )
        {
            const uint32_t bitOffset = MATERIAL_BLENDING_FLAG_BIT_COUNT * i;

            switch( blendingTypes[ i ] )
            {
                case QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE: r |= MATERIAL_BLENDING_FLAG_OPAQUE << bitOffset; break;
                case QR_GEOMETRY_MATERIAL_BLEND_TYPE_ALPHA:  r |= MATERIAL_BLENDING_FLAG_ALPHA  << bitOffset; break;
                case QR_GEOMETRY_MATERIAL_BLEND_TYPE_ADD:    r |= MATERIAL_BLENDING_FLAG_ADD    << bitOffset; break;
                case QR_GEOMETRY_MATERIAL_BLEND_TYPE_SHADE:  r |= MATERIAL_BLENDING_FLAG_SHADE  << bitOffset; break;
                default: assert(0); break;
            }
        }

        return r;
    }

    uint32_t GetGeometryInstanceUploadFlags(
        const QrGeometryUploadInfo &info, VertexCollectorFilterTypeFlags geomFlags )
    {
        uint32_t flags = 0;

        if( info.flags & QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT )
        {
            flags |= GEOM_INST_FLAG_GENERATE_NORMALS;
        }

        if( info.flags & QR_GEOMETRY_UPLOAD_GENERATE_INVERTED_NORMALS_BIT )
        {
            flags |= GEOM_INST_FLAG_GENERATE_NORMALS;
            flags |= GEOM_INST_FLAG_INVERTED_NORMALS;
        }

        if( info.flags & QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT )
        {
            flags |= GEOM_INST_FLAG_EXACT_NORMALS;
        }

        if( info.flags & QR_GEOMETRY_UPLOAD_NO_MEDIA_CHANGE_ON_REFRACT_BIT )
        {
            flags |= GEOM_INST_FLAG_NO_MEDIA_CHANGE;
        }

        if( info.flags & QR_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_MULTIPLY_BIT )
        {
            flags |= GEOM_INST_FLAG_REFL_REFR_ALBEDO_MULT;
        }
        else if( info.flags & QR_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_ADD_BIT )
        {
            flags |= GEOM_INST_FLAG_REFL_REFR_ALBEDO_ADD;
        }

        if( info.flags & QR_GEOMETRY_UPLOAD_TURB_WARP_BIT )
        {
            flags |= GEOM_INST_FLAG_TURB_WARP;
        }

        if( info.flags & QR_GEOMETRY_UPLOAD_IGNORE_REFRACT_AFTER_REFRACT_BIT )
        {
            flags |= GEOM_INST_FLAG_IGNORE_REFRACT_AFTER;
        }

        if( geomFlags & VertexCollectorFilterTypeFlagBits::CF_STATIC_MOVABLE )
        {
            flags |= GEOM_INST_FLAG_IS_MOVABLE;
        }

        switch( info.passThroughType )
        {
            case QR_GEOMETRY_PASS_THROUGH_TYPE_MIRROR:
                flags |= GEOM_INST_FLAG_REFLECT;
                break;

            case QR_GEOMETRY_PASS_THROUGH_TYPE_PORTAL:
                if( info.pPortalIndex )
                {
                    flags |= GEOM_INST_FLAG_PORTAL;
                }
                break;

            case QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_ONLY_REFLECT:
                flags |= GEOM_INST_FLAG_MEDIA_TYPE_WATER;
                flags |= GEOM_INST_FLAG_REFLECT;
                break;

            case QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT:
                flags |= GEOM_INST_FLAG_MEDIA_TYPE_WATER;
                flags |= GEOM_INST_FLAG_REFLECT;
                flags |= GEOM_INST_FLAG_REFRACT;
                break;

            case QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT:
                flags |= GEOM_INST_FLAG_MEDIA_TYPE_GLASS;
                flags |= GEOM_INST_FLAG_REFLECT;
                flags |= GEOM_INST_FLAG_REFRACT;
                break;

            case QR_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT:
                flags |= GEOM_INST_FLAG_MEDIA_TYPE_ACID;
                flags |= GEOM_INST_FLAG_REFLECT;
                flags |= GEOM_INST_FLAG_REFRACT;
                break;

            default: break;
        }

        return flags;
    }
}

VertexCollector::VertexCollector( VkDevice                                  _device,
                                  const std::shared_ptr< MemoryAllocator >& _allocator,
                                  std::shared_ptr< GeomInfoManager >        _geomInfoManager,
                                  VkDeviceSize                              _bufferSize,
                                  VertexCollectorFilterTypeFlags            _filters )
    : device( _device )
    , filtersFlags( _filters )
    , geomInfoMgr( std::move( _geomInfoManager ) )
    , curVertexCount( 0 )
    , curIndexCount( 0 )
    , curPrimitiveCount( 0 )
    , curTransformCount( 0 )
    , mappedVertexData( nullptr )
    , mappedIndexData( nullptr )
    , mappedTransformData( nullptr )
{
    assert( filtersFlags != 0 );

    const bool isDynamic = filtersFlags & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC;

    vertBuffer       = std::make_shared< Buffer >();
    indexBuffer      = std::make_shared< Buffer >();
    transformsBuffer = std::make_shared< Buffer >();

    const VkBufferUsageFlags transferUsage =
        isDynamic ? VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                  : VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    vertBuffer->Init(
        _allocator, _bufferSize,
        transferUsage | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        isDynamic ? "Dynamic Vertices data buffer" : "Static Vertices data buffer");

    indexBuffer->Init(
        _allocator, INDEX_BUFFER_SIZE,
        transferUsage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        isDynamic ? "Dynamic Index data buffer" : "Static Index data buffer");

    transformsBuffer->Init(
        _allocator, TRANSFORM_BUFFER_SIZE,
        transferUsage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        isDynamic ? "Dynamic BLAS transforms buffer" : "Static BLAS transforms buffer");

    InitStagingBuffers( _allocator );
    InitFilters( filtersFlags );
}

VertexCollector::VertexCollector( const std::shared_ptr< const VertexCollector >& _src,
                                  const std::shared_ptr< MemoryAllocator >&       _allocator )
    : device( _src->device )
    , filtersFlags( _src->filtersFlags )
    , vertBuffer( _src->vertBuffer )
    , indexBuffer( _src->indexBuffer )
    , transformsBuffer( _src->transformsBuffer )
    , geomInfoMgr( _src->geomInfoMgr )
    , curVertexCount( 0 )
    , curIndexCount( 0 )
    , curPrimitiveCount( 0 )
    , curTransformCount( 0 )
    , mappedVertexData( nullptr )
    , mappedIndexData( nullptr )
    , mappedTransformData( nullptr )
{
    InitStagingBuffers( _allocator );
    InitFilters( filtersFlags );
}

void VertexCollector::InitStagingBuffers( const std::shared_ptr< MemoryAllocator >& allocator )
{
    assert( vertBuffer && vertBuffer->GetSize() > 0 );
    assert( indexBuffer && indexBuffer->GetSize() > 0 );
    assert( transformsBuffer && transformsBuffer->GetSize() > 0 );
    assert( geomInfoMgr );

    stagingVertBuffer.Init( allocator,
                            vertBuffer->GetSize(),
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            filtersFlags & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC
                                ? "Dynamic Vertices data staging buffer"
                                : "Static Vertices data staging buffer" );

    stagingIndexBuffer.Init( allocator,
                             indexBuffer->GetSize(),
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             filtersFlags & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC
                                 ? "Dynamic Index data staging buffer"
                                 : "Static Index data staging buffer" );

    stagingTransformsBuffer.Init( allocator,
                                  transformsBuffer->GetSize(),
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  filtersFlags & VertexCollectorFilterTypeFlagBits::CF_DYNAMIC
                                      ? "Dynamic BLAS transforms staging buffer"
                                      : "Static BLAS transforms staging buffer" );

    mappedVertexData    = static_cast< ShVertex* >( stagingVertBuffer.Map() );
    mappedIndexData     = static_cast< uint32_t* >( stagingIndexBuffer.Map() );
    mappedTransformData = static_cast< VkTransformMatrixKHR* >( stagingTransformsBuffer.Map() );
}

VertexCollector::~VertexCollector()
{
    stagingVertBuffer.TryUnmap();
    stagingIndexBuffer.TryUnmap();
    stagingTransformsBuffer.TryUnmap();
}

void VertexCollector::BeginCollecting( bool isStatic )
{
    assert( curVertexCount == 0 && curIndexCount == 0 && curPrimitiveCount == 0 );
    assert( ( isStatic && geomInfoMgr->GetStaticCount() == 0 ) ||
            ( !isStatic && geomInfoMgr->GetDynamicCount() == 0 ) );
    assert( GetAllGeometryCount() == 0 );
}

uint32_t VertexCollector::AddGeometry( uint32_t                         frameIndex,
                                       const QrGeometryUploadInfo&      info,
                                       std::span< MaterialTextures, 3 > materials )
{
    typedef VertexCollectorFilterTypeFlagBits FT;
    const VertexCollectorFilterTypeFlags      geomFlags =
        VertexCollectorFilterTypeFlags_GetForGeometry( info );

    if( GetGeometryCount( geomFlags ) + 1 >=
        VertexCollectorFilterTypeFlags_GetAmountInGlobalArray( geomFlags ) )
    {
        assert( false && "Too many geometries in a group" );
        return UINT32_MAX;
    }

    const bool collectStatic = geomFlags & ( FT::CF_STATIC_NON_MOVABLE | FT::CF_STATIC_MOVABLE );

    const uint32_t maxVertexCount =
        collectStatic ? MAX_STATIC_VERTEX_COUNT : MAX_DYNAMIC_VERTEX_COUNT;

    const uint32_t vertIndex      = AlignUpBy3( curVertexCount );
    const uint32_t indIndex       = AlignUpBy3( curIndexCount );
    const uint32_t transformIndex = curTransformCount;

    const bool     useIndices     = info.indexCount != 0 && info.pIndices != nullptr;
    const uint32_t primitiveCount = useIndices ? info.indexCount / 3 : info.vertexCount / 3;

    curVertexCount = vertIndex + info.vertexCount;
    curIndexCount  = indIndex + ( useIndices ? info.indexCount : 0 );
    curPrimitiveCount += primitiveCount;
    curTransformCount += 1;

    if( curVertexCount >= maxVertexCount )
    {
        assert( 0 );
        return UINT32_MAX;
    }

    if( curIndexCount >= MAX_INDEXED_PRIMITIVE_COUNT * 3 )
    {
        assert( 0 );
        return UINT32_MAX;
    }

    if( geomInfoMgr->GetCount() + 1 >= MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT )
    {
        assert( 0 );
        return UINT32_MAX;
    }

    assert( stagingVertBuffer.IsMapped() );
    CopyDataToStaging( info, vertIndex );

    if( useIndices )
    {
        assert( stagingIndexBuffer.IsMapped() );
        memcpy( mappedIndexData + indIndex, info.pIndices, info.indexCount * sizeof( uint32_t ) );
    }

    static_assert( sizeof( QrTransform ) == sizeof( VkTransformMatrixKHR ),
                   "QrTransform and VkTransformMatrixKHR must have the same structure to be used "
                   "in AS building" );
    memcpy( mappedTransformData + transformIndex, &info.transform, sizeof( VkTransformMatrixKHR ) );

    const VkDeviceAddress vertexDataDeviceAddress =
        vertBuffer->GetAddress() + vertIndex * sizeof( ShVertex ) + offsetof( ShVertex, position );

    VkAccelerationStructureGeometryKHR geom = {};
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = geomFlags & FT::PT_OPAQUE ? VK_GEOMETRY_OPAQUE_BIT_KHR
                                           : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;

    VkAccelerationStructureGeometryTrianglesDataKHR& trData = geom.geometry.triangles;
    trData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    trData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    trData.maxVertex = info.vertexCount;
    trData.vertexData.deviceAddress = vertexDataDeviceAddress;
    trData.vertexStride = sizeof( ShVertex );
    trData.transformData.deviceAddress =
        transformsBuffer->GetAddress() + transformIndex * sizeof( VkTransformMatrixKHR );

    if( useIndices )
    {
        const VkDeviceAddress indexDataDeviceAddress =
            indexBuffer->GetAddress() + indIndex * sizeof( uint32_t );

        trData.indexType = VK_INDEX_TYPE_UINT32;
        trData.indexData.deviceAddress = indexDataDeviceAddress;
    }
    else
    {
        trData.indexType = VK_INDEX_TYPE_NONE_KHR;
        trData.indexData = {};
    }

    const uint32_t localIndex = PushGeometry( geomFlags, geom );

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {};
    rangeInfo.primitiveCount = primitiveCount;
    rangeInfo.primitiveOffset = 0;
    rangeInfo.firstVertex = 0;
    rangeInfo.transformOffset = 0;
    PushRangeInfo( geomFlags, rangeInfo );

    PushPrimitiveCount( geomFlags, primitiveCount );

    ShGeometryInstance geomInfo = {};
    geomInfo.baseVertexIndex    = vertIndex;
    geomInfo.baseIndexIndex     = useIndices ? indIndex : UINT32_MAX;
    geomInfo.vertexCount        = info.vertexCount;
    geomInfo.indexCount         = useIndices ? info.indexCount : UINT32_MAX;
    geomInfo.defaultRoughness   = std::clamp( info.defaultRoughness, 0.0f, 1.0f );
    geomInfo.defaultMetallicity = std::clamp( info.defaultMetallicity, 0.0f, 1.0f );
    geomInfo.defaultEmission    = std::clamp( info.defaultEmission, 0.0f, 1.0f );

    Matrix::ToMat4Transposed( geomInfo.model, info.transform );

    geomInfo.flags = GetMaterialsBlendFlags( info.layerBlendingTypes, MATERIALS_MAX_LAYER_COUNT ) |
                     GetGeometryInstanceUploadFlags( info, geomFlags );

    static_assert( sizeof( QrLayeredMaterial ) / sizeof( QrMaterial ) == MATERIALS_MAX_LAYER_COUNT,
                   "Layer count mismatch with ShGeometryInstance" );
    {
        geomInfo.materials0A = materials[ 0 ].indices[ 0 ];
        geomInfo.materials0B = materials[ 0 ].indices[ 1 ];
        geomInfo.materials0C = materials[ 0 ].indices[ 2 ];

        geomInfo.materials1A = materials[ 1 ].indices[ 0 ];
        geomInfo.materials1B = materials[ 1 ].indices[ 1 ];

        geomInfo.materials2A = materials[ 2 ].indices[ 0 ];
        geomInfo.materials2B = materials[ 2 ].indices[ 1 ];
    }

    for( uint32_t layer = 0; layer < MATERIALS_MAX_LAYER_COUNT; layer++ )
    {
        memcpy( geomInfo.materialColors[ layer ],
                info.layerColors[ layer ].data,
                sizeof( info.layerColors[ layer ].data ) );
    }

    geomInfo.portalIndex = info.pPortalIndex ? *info.pPortalIndex : PORTAL_INDEX_NONE;

    const uint32_t simpleIndex = geomInfoMgr->WriteGeomInfo( frameIndex, info.uniqueID, localIndex, geomFlags, geomInfo );

    if( collectStatic )
    {
        const uint32_t layerTextureCounts[ MATERIALS_MAX_LAYER_COUNT ] = { 3, 2, 2 };

        for( uint32_t layer = 0; layer < MATERIALS_MAX_LAYER_COUNT; layer++ )
        {
            bool hasTexture = false;

            for( uint32_t texture = 0; texture < layerTextureCounts[ layer ]; texture++ )
            {
                if( materials[ layer ].indices[ texture ] != EMPTY_TEXTURE_INDEX )
                {
                    hasTexture = true;
                    break;
                }
            }

            if( hasTexture )
            {
                AddMaterialDependency( simpleIndex, layer, info.geomMaterial.layerMaterials[ layer ] );
            }
        }

        simpleIndexToTransformIndex[ simpleIndex ] = transformIndex;
    }

    return simpleIndex;
}

void VertexCollector::CopyDataToStaging(const QrGeometryUploadInfo &info, uint32_t vertIndex)
{
    assert( ( vertIndex + info.vertexCount ) * sizeof( ShVertex ) < vertBuffer->GetSize() );

    ShVertex* const pDst = &mappedVertexData[ vertIndex ];

    static_assert( std::is_same_v< decltype( info.pVertices ), const QrVertex* > );
    static_assert( sizeof( ShVertex )                   == sizeof( QrVertex ) );
    static_assert( offsetof( ShVertex, position )       == offsetof( QrVertex, position ) );
    static_assert( offsetof( ShVertex, normal )         == offsetof( QrVertex, normal ) );
    static_assert( offsetof( ShVertex, texCoord )       == offsetof( QrVertex, texCoord ) );
    static_assert( offsetof( ShVertex, texCoordLayer1 ) == offsetof( QrVertex, texCoordLayer1 ) );
    static_assert( offsetof( ShVertex, texCoordLayer2 ) == offsetof( QrVertex, texCoordLayer2 ) );
    static_assert( offsetof( ShVertex, packedColor )    == offsetof( QrVertex, packedColor ) );

    memcpy( pDst, info.pVertices, info.vertexCount * sizeof( ShVertex ) );
}

void VertexCollector::EndCollecting()
{
}

void VertexCollector::Reset()
{
    curVertexCount    = 0;
    curIndexCount     = 0;
    curPrimitiveCount = 0;
    curTransformCount = 0;

    simpleIndexToTransformIndex.clear();

    materialDependencies.clear();

    for( auto& f : filters )
    {
        f.second->Reset();
    }
}

bool VertexCollector::CopyVertexDataFromStaging( VkCommandBuffer cmd )
{
    if( curVertexCount == 0 )
    {
        return false;
    }

    const VkBufferCopy info =
    {
        .srcOffset = 0,
        .dstOffset = 0,
        .size      = curVertexCount * sizeof( ShVertex ),
    };

    vkCmdCopyBuffer( cmd, stagingVertBuffer.GetBuffer(), vertBuffer->GetBuffer(), 1, &info );

    return true;
}

bool VertexCollector::CopyIndexDataFromStaging( VkCommandBuffer cmd )
{
    if( curIndexCount == 0 )
    {
        return false;
    }

    const VkBufferCopy info =
    {
        .srcOffset = 0,
        .dstOffset = 0,
        .size      = curIndexCount * sizeof( uint32_t ),
    };

    vkCmdCopyBuffer( cmd, stagingIndexBuffer.GetBuffer(), indexBuffer->GetBuffer(), 1, &info );

    return true;
}

bool VertexCollector::CopyTransformsFromStaging( VkCommandBuffer cmd )
{
    if( curTransformCount == 0 )
    {
        return false;
    }

    const VkBufferCopy info =
    {
        .srcOffset = 0,
        .dstOffset = 0,
        .size      = curTransformCount * sizeof( VkTransformMatrixKHR ),
    };

    vkCmdCopyBuffer( cmd, stagingTransformsBuffer.GetBuffer(), transformsBuffer->GetBuffer(), 1, &info );

    return true;
}

bool VertexCollector::CopyFromStaging( VkCommandBuffer cmd )
{
    const bool vertCopied = CopyVertexDataFromStaging( cmd );
    const bool indexCopied = CopyIndexDataFromStaging( cmd );
    const bool transformsCopied = CopyTransformsFromStaging( cmd );

    std::array< VkBufferMemoryBarrier, 2 > barriers     = {};
    uint32_t                               barrierCount = 0;

    if( vertCopied )
    {
        VkBufferMemoryBarrier& barrier = barriers[ barrierCount ];
        barrierCount++;

        barrier = {};
        barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.buffer              = vertBuffer->GetBuffer();
        barrier.offset              = 0;
        barrier.size                = curVertexCount * sizeof( ShVertex );
    }

    if( indexCopied )
    {
        VkBufferMemoryBarrier& barrier = barriers[ barrierCount ];
        barrierCount++;

        barrier = {};
        barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.buffer              = indexBuffer->GetBuffer();
        barrier.offset              = 0;
        barrier.size                = curIndexCount * sizeof( uint32_t );
    }

    if( barrierCount > 0 )
    {
        vkCmdPipelineBarrier( cmd,
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                  VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                              0,
                              0,
                              nullptr,
                              barrierCount,
                              barriers.data(),
                              0,
                              nullptr );
    }

    if( transformsCopied )
    {
        VkBufferMemoryBarrier barrier = {};
        barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask       = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        barrier.buffer              = transformsBuffer->GetBuffer();
        barrier.size                = curTransformCount * sizeof( VkTransformMatrixKHR );

        vkCmdPipelineBarrier( cmd,
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                              0,
                              0,
                              nullptr,
                              1,
                              &barrier,
                              0,
                              nullptr );
    }

    return vertCopied || indexCopied || transformsCopied;
}

void VertexCollector::UpdateTransform( uint32_t                     simpleIndex,
                                       const QrUpdateTransformInfo& updateInfo )
{
    if( simpleIndex >= MAX_BOTTOM_LEVEL_GEOMETRIES_COUNT )
    {
        assert( 0 );
        return;
    }

    assert( mappedTransformData != nullptr );

    static_assert( sizeof( QrTransform ) == sizeof( VkTransformMatrixKHR ),
                   "QrTransform and VkTransformMatrixKHR must have the same structure to be used "
                   "in AS building" );
    memcpy( mappedTransformData + simpleIndexToTransformIndex[ simpleIndex ],
            &updateInfo.transform,
            sizeof( VkTransformMatrixKHR ) );

    geomInfoMgr->WriteStaticGeomInfoTransform(
        simpleIndex, updateInfo.movableStaticUniqueID, updateInfo.transform );
}

void VertexCollector::UpdateTexCoords( uint32_t                     simpleIndex,
                                       const QrUpdateTexCoordsInfo& texCoordsInfo,
                                       bool                         isStatic )
{
    assert( isStatic );
    assert( mappedVertexData != nullptr );

    const uint32_t maxVertexCount = isStatic ? MAX_STATIC_VERTEX_COUNT : MAX_DYNAMIC_VERTEX_COUNT;

    const uint32_t globalVertIndex = geomInfoMgr->GetStaticGeomBaseVertexIndex( simpleIndex );
    const uint32_t dstVertIndex    = globalVertIndex + texCoordsInfo.vertexOffset;

    if( dstVertIndex + texCoordsInfo.vertexCount >= maxVertexCount )
    {
        assert( 0 );
        return;
    }

    assert( 0 );
}

void VertexCollector::AddMaterialDependency( uint32_t simpleIndex,
                                             uint32_t layer,
                                             uint32_t materialIndex )
{
    if( materialIndex != QR_NO_MATERIAL )
    {
        materialDependencies[ materialIndex ].push_back( { simpleIndex, layer } );
    }
}

void VertexCollector::OnMaterialChange( uint32_t materialIndex, const MaterialTextures& newInfo )
{
    for( const auto& p : materialDependencies[ materialIndex ] )
    {
        geomInfoMgr->WriteStaticGeomInfoMaterials( p.simpleIndex, p.layer, newInfo );
    }
}

VkBuffer VertexCollector::GetVertexBuffer() const
{
    return vertBuffer->GetBuffer();
}

VkBuffer VertexCollector::GetIndexBuffer() const
{
    return indexBuffer->GetBuffer();
}

VkDeviceAddress VertexCollector::GetVertexBufferAddress() const
{
    return vertBuffer->GetAddress();
}

VkDeviceAddress VertexCollector::GetIndexBufferAddress() const
{
    return indexBuffer->GetAddress();
}

VkDeviceAddress VertexCollector::GetTransformsBufferAddress() const
{
    return transformsBuffer->GetAddress();
}

VkDeviceSize VertexCollector::GetVertexBufferSize() const
{
    return vertBuffer->GetSize();
}

VkDeviceSize VertexCollector::GetIndexBufferSize() const
{
    return indexBuffer->GetSize();
}

const VkTransformMatrixKHR *VertexCollector::GetTransformsStaging() const
{
    return mappedTransformData;
}

VkBuffer VertexCollector::GetStagingVertexBuffer() const
{
    return stagingVertBuffer.GetBuffer();
}

VkBuffer VertexCollector::GetStagingIndexBuffer() const
{
    return stagingIndexBuffer.GetBuffer();
}

std::vector<VertexCollector::GeometryDrawInfo> VertexCollector::GetGeometryDrawInfos() const
{
    std::vector<GeometryDrawInfo> result;

    if( vertBuffer == nullptr || indexBuffer == nullptr )
    {
        return result;
    }

    const VkDeviceAddress vertexBase     = vertBuffer->GetAddress() + offsetof( ShVertex, position );
    const VkDeviceAddress indexBase      = indexBuffer->GetAddress();
    const VkDeviceAddress transformsBase = transformsBuffer->GetAddress();

    for( const auto& [filter, f] : filters )
    {
        if( filter & (VertexCollectorFilterTypeFlags)VertexCollectorFilterTypeFlagBits::PV_WORLD_2 )
        {
            continue;
        }

        const auto& geoms  = f->GetASGeometries();
        const auto& ranges = f->GetASBuildRangeInfos();

        const size_t count = std::min( geoms.size(), ranges.size() );

        for( size_t i = 0; i < count; i++ )
        {
            const auto& tr = geoms[ i ].geometry.triangles;

            GeometryDrawInfo info = {};
            info.vertexBuffer = vertBuffer->GetBuffer();
            info.indexBuffer  = indexBuffer->GetBuffer();
            info.baseVertex   = static_cast< uint32_t >( ( tr.vertexData.deviceAddress - vertexBase ) / sizeof( ShVertex ) );
            info.indexCount   = ranges[ i ].primitiveCount * 3;

            if( tr.indexType == VK_INDEX_TYPE_UINT32 )
            {
                info.firstIndex = static_cast< uint32_t >( ( tr.indexData.deviceAddress - indexBase ) / sizeof( uint32_t ) );
            }
            else
            {
                info.firstIndex = 0;
            }

            const VkDeviceAddress transformAddr = tr.transformData.deviceAddress;
            const uint32_t        transformIndex =
                ( transformsBase != 0 && transformAddr >= transformsBase )
                    ? static_cast< uint32_t >( ( transformAddr - transformsBase ) / sizeof( VkTransformMatrixKHR ) )
                    : 0;

            const QrTransform& t = reinterpret_cast< const QrTransform& >( mappedTransformData[ transformIndex ] );
            Matrix::ToMat4Transposed( info.model, t );

            result.push_back( info );
        }
    }

    return result;
}

const std::vector< uint32_t >& VertexCollector::GetPrimitiveCounts(
    VertexCollectorFilterTypeFlags filter ) const
{
    const auto f = filters.find( filter );
    assert( f != filters.end() );

    return f->second->GetPrimitiveCounts();
}

const std::vector< VkAccelerationStructureGeometryKHR >& VertexCollector::GetASGeometries(
    VertexCollectorFilterTypeFlags filter ) const
{
    const auto f = filters.find( filter );
    assert( f != filters.end() );

    return f->second->GetASGeometries();
}

const std::vector< VkAccelerationStructureBuildRangeInfoKHR >& VertexCollector::
    GetASBuildRangeInfos( VertexCollectorFilterTypeFlags filter ) const
{
    const auto f = filters.find( filter );
    assert( f != filters.end() );

    return f->second->GetASBuildRangeInfos();
}

bool VertexCollector::AreGeometriesEmpty( VertexCollectorFilterTypeFlags flags ) const
{
    for( const auto& p : filters )
    {
        const auto& f = p.second;

        if( ( f->GetFilter() & flags ) && f->GetGeometryCount() > 0 )
        {
            return false;
        }
    }

    return true;
}

bool VertexCollector::AreGeometriesEmpty( VertexCollectorFilterTypeFlagBits type ) const
{
    return AreGeometriesEmpty( ( VertexCollectorFilterTypeFlags )type );
}

void VertexCollector::InsertVertexPreprocessBeginBarrier( VkCommandBuffer cmd )
{
}

void VertexCollector::InsertVertexPreprocessFinishBarrier( VkCommandBuffer cmd )
{
    std::array< VkBufferMemoryBarrier, 2 > barriers     = {};
    uint32_t                               barrierCount = 0;

    if( curVertexCount > 0 )
    {
        VkBufferMemoryBarrier& barrier = barriers[ barrierCount ];
        barrierCount++;

        barrier = {};
        barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
        barrier.buffer = vertBuffer->GetBuffer();
        barrier.offset = 0;
        barrier.size   = curVertexCount * sizeof( ShVertex );
    }

    if( curIndexCount > 0 )
    {
        VkBufferMemoryBarrier& barrier = barriers[ barrierCount ];
        barrierCount++;

        barrier = {};
        barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
        barrier.buffer = indexBuffer->GetBuffer();
        barrier.offset = 0;
        barrier.size   = curIndexCount * sizeof( uint32_t );
    }

    if( barrierCount == 0 )
    {
        return;
    }

    vkCmdPipelineBarrier( cmd,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                              VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                          0,
                          0,
                          nullptr,
                          barrierCount,
                          barriers.data(),
                          0,
                          nullptr );
}

uint32_t VertexCollector::PushGeometry( VertexCollectorFilterTypeFlags            type,
                                        const VkAccelerationStructureGeometryKHR& geom )
{
    assert( filters.find( type ) != filters.end() );

    return filters[ type ]->PushGeometry( type, geom );
}

void VertexCollector::PushPrimitiveCount( VertexCollectorFilterTypeFlags type, uint32_t primCount )
{
    assert( filters.find( type ) != filters.end() );

    filters[ type ]->PushPrimitiveCount( type, primCount );
}

void VertexCollector::PushRangeInfo( VertexCollectorFilterTypeFlags                  type,
                                     const VkAccelerationStructureBuildRangeInfoKHR& rangeInfo )
{
    assert( filters.find( type ) != filters.end() );

    filters[ type ]->PushRangeInfo( type, rangeInfo );
}

uint32_t VertexCollector::GetGeometryCount( VertexCollectorFilterTypeFlags type )
{
    assert( filters.find( type ) != filters.end() );

    return filters[ type ]->GetGeometryCount();
}

uint32_t VertexCollector::GetAllGeometryCount() const
{
    uint32_t count = 0;

    for( const auto& f : filters )
    {
        count += f.second->GetGeometryCount();
    }

    return count;
}

uint32_t VertexCollector::GetCurrentVertexCount() const
{
    return curVertexCount;
}

uint32_t VertexCollector::GetCurrentIndexCount() const
{
    return curIndexCount;
}

void VertexCollector::AddFilter( VertexCollectorFilterTypeFlags filterGroup )
{
    if( filterGroup == ( VertexCollectorFilterTypeFlags )0 )
    {
        return;
    }

    assert( filters.find( filterGroup ) == filters.end() );

    filters[ filterGroup ] = std::make_shared< VertexCollectorFilter >( filterGroup );
}

void VertexCollector::InitFilters( VertexCollectorFilterTypeFlags flags )
{
    typedef VertexCollectorFilterTypeFlags    FL;

    VertexCollectorFilterTypeFlags_IterateOverFlags( [ this, flags ]( FL f ) {
        if( ( flags & f ) == f )
        {
            AddFilter( f );
        }
    } );
}
