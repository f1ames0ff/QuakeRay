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

#include "RasterizedDataCollector.h"

#include <algorithm>

#include "Utils.h"
#include "RgException.h"
#include "Generated/ShaderCommonC.h"

using namespace vkpt;

namespace
{
    struct VertexAttribute
    {
        uint32_t location;
        VkFormat format;
        size_t   offset;
    };

    void FillVertexAttributes(
        const VertexAttribute *attrs, size_t attrCount,
        VkVertexInputAttributeDescription *outAttrs, uint32_t *outAttrsCount)
    {
        for (size_t i = 0; i < attrCount; i++)
        {
            outAttrs[i].binding = 0;
            outAttrs[i].location = attrs[i].location;
            outAttrs[i].format = attrs[i].format;
            outAttrs[i].offset = (uint32_t)attrs[i].offset;
        }

        *outAttrsCount = (uint32_t)attrCount;
    }

    bool IsWorld(RgRasterizedGeometryRenderType type)
    {
        return type == RG_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT;
    }

    bool IsSwapchain(RgRasterizedGeometryRenderType type)
    {
        return type == RG_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN;
    }

    bool IsSky(RgRasterizedGeometryRenderType type)
    {
        return type == RG_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY;
    }

    VkViewport ToVkViewport(const RgViewport &v)
    {
        return VkViewport{
            .x        = v.x,
            .y        = v.y,
            .width    = v.width,
            .height   = v.height,
            .minDepth = v.minDepth,
            .maxDepth = v.maxDepth,
        };
    }

    uint32_t ResolveTextureIndex_AlbedoAlpha(
        const vkpt::TextureManager &manager, const RgRasterizedGeometryUploadInfo &info)
    {
        if (info.material == RG_NO_MATERIAL)
        {
            return EMPTY_TEXTURE_INDEX;
        }

        return manager.GetMaterialTextures(info.material).indices[MATERIAL_ALBEDO_ALPHA_INDEX];
    }

    uint32_t ResolveTextureIndex_RME(
        const vkpt::TextureManager &manager, const RgRasterizedGeometryUploadInfo &info)
    {
        if (info.material == RG_NO_MATERIAL)
        {
            return EMPTY_TEXTURE_INDEX;
        }

        if (!IsWorld(info.renderType))
        {
            return EMPTY_TEXTURE_INDEX;
        }

        return manager.GetMaterialTextures(info.material).indices[MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX];
    }
}

void RasterizedDataCollector::GetVertexLayout(
    VkVertexInputAttributeDescription *outAttrs, uint32_t *outAttrsCount)
{
    const VertexAttribute attrs[] =
    {
        { 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RgVertex, position)    },
        { 1, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(RgVertex, packedColor) },
        { 2, VK_FORMAT_R32G32_SFLOAT,    offsetof(RgVertex, texCoord)    },
    };

    FillVertexAttributes(attrs, std::size(attrs), outAttrs, outAttrsCount);
}

uint32_t RasterizedDataCollector::GetVertexStride()
{
    return static_cast<uint32_t>(sizeof(RgVertex));
}

void RasterizedDataCollector::GetSmokeVertexLayout(
    VkVertexInputAttributeDescription *outAttrs, uint32_t *outAttrsCount)
{
    const VertexAttribute attrs[] =
    {
        { 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RgVertex, position)     },
        { 1, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(RgVertex, packedColor)  },
        { 2, VK_FORMAT_R32G32_SFLOAT,    offsetof(RgVertex, texCoord)     },
        { 3, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RgVertex, normal)       },
        { 4, VK_FORMAT_R32G32_SFLOAT,    offsetof(RgVertex, texCoordLayer1) },
        { 5, VK_FORMAT_R32_UINT,         offsetof(RgVertex, cluster)      },
    };

    FillVertexAttributes(attrs, std::size(attrs), outAttrs, outAttrsCount);
}

RasterizedDataCollector::RasterizedDataCollector( VkDevice                            _device,
                                                  std::shared_ptr< MemoryAllocator >& _allocator,
                                                  std::shared_ptr< TextureManager >   _textureMgr,
                                                  uint32_t _maxVertexCount,
                                                  uint32_t _maxIndexCount )
    : device( _device )
    , textureMgr( std::move( _textureMgr ) )
    , curVertexCount( 0 )
    , curIndexCount( 0 )
{
    vertexBuffer = std::make_shared<AutoBuffer>(_device, _allocator);
    indexBuffer = std::make_shared<AutoBuffer>(_device, _allocator);

    _maxVertexCount = std::max(_maxVertexCount, 64u);
    _maxIndexCount = std::max(_maxIndexCount, 64u);

    vertexBuffer->Create(_maxVertexCount * sizeof(RgVertex),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         "Rasterizer vertex buffer");
    indexBuffer->Create(_maxIndexCount * sizeof(uint32_t),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        "Rasterizer index buffer");
}

RasterizedDataCollector::~RasterizedDataCollector()
{
}

void RasterizedDataCollector::AddGeometry(uint32_t frameIndex,
                                          const RgRasterizedGeometryUploadInfo &info,
                                          const float *pViewProjection, const RgViewport *pViewport)
{
    assert(info.vertexCount > 0);
    assert(info.pVertices != nullptr);

    if (IsSwapchain(info.renderType))
    {
        if (info.pipelineState & RG_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST)
        {
            assert(0);
            return;
        }

        if (info.pipelineState & RG_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE)
        {
            assert(0);
            return;
        }
    }

    if (IsSky(info.renderType))
    {
        if (pViewProjection != nullptr || pViewport != nullptr)
        {
            throw RgException(RG_CANT_UPLOAD_RASTERIZED_GEOMETRY, "pViewProjection and pViewport must be null if renderType is RG_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY");
        }
    }

    if (curVertexCount + info.vertexCount >= vertexBuffer->GetSize() / sizeof(RgVertex))
    {
        assert(0 && "Increase the size of \"rasterizedMaxVertexCount\". Vertex buffer size reached the limit.");
        return;
    }

    if (curIndexCount + info.indexCount >= indexBuffer->GetSize() / sizeof(uint32_t))
    {
        assert(0 && "Increase the size of \"rasterizedMaxIndexCount\". Index buffer size reached the limit.");
        return;
    }

    DrawInfo &drawInfo = PushInfo(info.renderType);

    ShVertex* const vertsBase   = static_cast< ShVertex* >( vertexBuffer->GetMapped( frameIndex ) );
    uint32_t* const indicesBase = static_cast< uint32_t* >( indexBuffer->GetMapped( frameIndex ) );

    drawInfo = {
        .transform            = info.transform,
        .viewProj             = IfNotNull( pViewProjection, Float16D( pViewProjection ) ),
        .viewport             = IfNotNull( pViewport, ToVkViewport( *pViewport ) ),
        .color                = Float4D( info.color.data ),
        .textureIndex         = ResolveTextureIndex_AlbedoAlpha( *textureMgr, info ),
        .emissionTextureIndex = ResolveTextureIndex_RME( *textureMgr, info ),
        .pipelineState        = info.pipelineState,
        .blendFuncSrc         = info.blendFuncSrc,
        .blendFuncDst         = info.blendFuncDst,
        .smokeNoise           = Float4D( info.smokeNoise.data ),
        .smokeLook            = Float4D( info.smokeLook.data ),
    };

    CopyFromArrayOfStructs( info, &vertsBase[ curVertexCount ] );

    drawInfo.vertexCount = info.vertexCount;
    drawInfo.firstVertex  = static_cast< uint32_t >( curVertexCount );
    curVertexCount += info.vertexCount;

    if( info.indexCount != 0 && info.pIndices != nullptr )
    {
        if( curIndexCount + info.indexCount >= indexBuffer->GetSize() / sizeof( uint32_t ) )
        {
            assert( 0 );
            return;
        }

        memcpy( &indicesBase[ curIndexCount ], info.pIndices, info.indexCount * sizeof( uint32_t ) );

        drawInfo.indexCount = info.indexCount;
        drawInfo.firstIndex = static_cast< uint32_t >( curIndexCount );

        curIndexCount += info.indexCount;
    }
}

RasterizedDataCollector::DrawInfo& RasterizedDataCollector::PushInfo(
    RgRasterizedGeometryRenderType renderType )
{
    if( renderType == RG_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT )
    {
        return rasterDrawInfos.emplace_back();
    }

    if( renderType == RG_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN )
    {
        return swapchainDrawInfos.emplace_back();
    }

    if( renderType == RG_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY )
    {
        return skyDrawInfos.emplace_back();
    }

    throw RgException( RG_GRAPHICS_API_ERROR, "RasterizedDataCollector::PushInfo error" );
}

void RasterizedDataCollector::CopyFromArrayOfStructs(
    const RgRasterizedGeometryUploadInfo &info, ShVertex *dstVerts)
{
    assert(info.pVertices != nullptr);

    static_assert(std::is_same_v<decltype(info.pVertices), const RgVertex * >);
    static_assert(sizeof(ShVertex)                      == sizeof(RgVertex));
    static_assert(offsetof(ShVertex, position)          == offsetof(RgVertex, position));
    static_assert(offsetof(ShVertex, normal)            == offsetof(RgVertex, normal));
    static_assert(offsetof(ShVertex, texCoord)          == offsetof(RgVertex, texCoord));
    static_assert(offsetof(ShVertex, texCoordLayer1)    == offsetof(RgVertex, texCoordLayer1));
    static_assert(offsetof(ShVertex, texCoordLayer2)    == offsetof(RgVertex, texCoordLayer2));
    static_assert(offsetof(ShVertex, packedColor)       == offsetof(RgVertex, packedColor));

    memcpy(dstVerts, info.pVertices, sizeof(RgVertex) * info.vertexCount);
}

void RasterizedDataCollector::Clear(uint32_t frameIndex)
{
    rasterDrawInfos.clear();
    swapchainDrawInfos.clear();
    skyDrawInfos.clear();

    curVertexCount = 0;
    curIndexCount = 0;
}

void RasterizedDataCollector::CopyFromStaging(VkCommandBuffer cmd, uint32_t frameIndex)
{
    vertexBuffer->CopyFromStaging(cmd, frameIndex, sizeof(RgVertex) * curVertexCount);
    indexBuffer->CopyFromStaging(cmd, frameIndex, sizeof(uint32_t) * curIndexCount);
}

VkBuffer RasterizedDataCollector::GetVertexBuffer() const
{
    return vertexBuffer->GetDeviceLocal();
}

VkBuffer RasterizedDataCollector::GetIndexBuffer() const
{
    return indexBuffer->GetDeviceLocal();
}

VkBuffer RasterizedDataCollector::GetVertexStagingBuffer(uint32_t frameIndex)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    return vertexBuffer->GetStaging(frameIndex);
}

VkBuffer RasterizedDataCollector::GetIndexStagingBuffer(uint32_t frameIndex)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    return indexBuffer->GetStaging(frameIndex);
}

VkDeviceSize RasterizedDataCollector::GetVertexBufferSize() const
{
    return vertexBuffer->GetSize();
}

VkDeviceSize RasterizedDataCollector::GetIndexBufferSize() const
{
    return indexBuffer->GetSize();
}

const std::vector< RasterizedDataCollector::DrawInfo >& RasterizedDataCollector::
    GetRasterDrawInfos() const
{
    return rasterDrawInfos;
}

const std::vector< RasterizedDataCollector::DrawInfo >& RasterizedDataCollector::
    GetSwapchainDrawInfos() const
{
    return swapchainDrawInfos;
}

const std::vector< RasterizedDataCollector::DrawInfo >& RasterizedDataCollector::
    GetSkyDrawInfos() const
{
    return skyDrawInfos;
}
