// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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
#include "QrException.h"
#include "Generated/ShaderCommonC.h"

using namespace qray;

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

    bool IsWorld(QrRasterizedGeometryRenderType type)
    {
        return type == QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT;
    }

    bool IsSwapchain(QrRasterizedGeometryRenderType type)
    {
        return type == QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN;
    }

    bool IsSky(QrRasterizedGeometryRenderType type)
    {
        return type == QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY;
    }

    VkViewport ToVkViewport(const QrViewport &v)
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

    VkRect2D ToVkRect2D(const QrRect2D &r)
    {
        return VkRect2D{
            .offset = { r.x, r.y },
            .extent = { r.width, r.height },
        };
    }

    uint32_t ResolveTextureIndex_AlbedoAlpha(
        const qray::TextureManager &manager, const QrRasterizedGeometryUploadInfo &info)
    {
        if (info.material == QR_NO_MATERIAL)
        {
            return EMPTY_TEXTURE_INDEX;
        }

        return manager.GetMaterialTextures(info.material).indices[MATERIAL_ALBEDO_ALPHA_INDEX];
    }

    uint32_t ResolveTextureIndex_RME(
        const qray::TextureManager &manager, const QrRasterizedGeometryUploadInfo &info)
    {
        if (info.material == QR_NO_MATERIAL)
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
        { 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(QrVertex, position)    },
        { 1, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(QrVertex, packedColor) },
        { 2, VK_FORMAT_R32G32_SFLOAT,    offsetof(QrVertex, texCoord)    },
    };

    FillVertexAttributes(attrs, std::size(attrs), outAttrs, outAttrsCount);
}

uint32_t RasterizedDataCollector::GetVertexStride()
{
    return static_cast<uint32_t>(sizeof(QrVertex));
}

void RasterizedDataCollector::GetSmokeVertexLayout(
    VkVertexInputAttributeDescription *outAttrs, uint32_t *outAttrsCount)
{
    const VertexAttribute attrs[] =
    {
        { 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(QrVertex, position)     },
        { 1, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(QrVertex, packedColor)  },
        { 2, VK_FORMAT_R32G32_SFLOAT,    offsetof(QrVertex, texCoord)     },
        { 3, VK_FORMAT_R32G32B32_SFLOAT, offsetof(QrVertex, normal)       },
        { 4, VK_FORMAT_R32G32_SFLOAT,    offsetof(QrVertex, texCoordLayer1) },
        { 5, VK_FORMAT_R32_UINT,         offsetof(QrVertex, cluster)      },
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

    vertexBuffer->Create(_maxVertexCount * sizeof(QrVertex),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         "Rasterizer vertex buffer");
    indexBuffer->Create(_maxIndexCount * sizeof(uint32_t),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        "Rasterizer index buffer");
}

RasterizedDataCollector::~RasterizedDataCollector()
{
}

bool RasterizedDataCollector::AddGeometry(uint32_t frameIndex,
                                          const QrRasterizedGeometryUploadInfo &info,
                                          const float *pViewProjection, const QrViewport *pViewport)
{
    assert(info.vertexCount > 0);
    assert(info.pVertices != nullptr);

    if (IsSwapchain(info.renderType))
    {
        if (info.pipelineState & QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST)
        {
            assert(0);
            return false;
        }

        if (info.pipelineState & QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE)
        {
            assert(0);
            return false;
        }
    }

    if (IsSky(info.renderType))
    {
        if (pViewProjection != nullptr || pViewport != nullptr)
        {
            throw QrException(QR_CANT_UPLOAD_RASTERIZED_GEOMETRY, "pViewProjection and pViewport must be null if renderType is QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY");
        }
    }

    if (curVertexCount + info.vertexCount >= vertexBuffer->GetSize() / sizeof(QrVertex))
    {
        return false;
    }

    if (curIndexCount + info.indexCount >= indexBuffer->GetSize() / sizeof(uint32_t))
    {
        return false;
    }

    DrawInfo &drawInfo = PushInfo(info.renderType);

    ShVertex* const vertsBase   = static_cast< ShVertex* >( vertexBuffer->GetMapped( frameIndex ) );
    uint32_t* const indicesBase = static_cast< uint32_t* >( indexBuffer->GetMapped( frameIndex ) );

    drawInfo = {
        .transform            = info.transform,
        .viewProj             = IfNotNull( pViewProjection, Float16D( pViewProjection ) ),
        .viewport             = IfNotNull( pViewport, ToVkViewport( *pViewport ) ),
        .scissor              = info.scissor.width > 0 ? std::optional< VkRect2D >( ToVkRect2D( info.scissor ) )
                                                      : std::nullopt,
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
            return false;
        }

        memcpy( &indicesBase[ curIndexCount ], info.pIndices, info.indexCount * sizeof( uint32_t ) );

        drawInfo.indexCount = info.indexCount;
        drawInfo.firstIndex = static_cast< uint32_t >( curIndexCount );

        curIndexCount += info.indexCount;
    }

    return true;
}

RasterizedDataCollector::DrawInfo& RasterizedDataCollector::PushInfo(
    QrRasterizedGeometryRenderType renderType )
{
    if( renderType == QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT )
    {
        return rasterDrawInfos.emplace_back();
    }

    if( renderType == QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN )
    {
        return swapchainDrawInfos.emplace_back();
    }

    if( renderType == QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY )
    {
        return skyDrawInfos.emplace_back();
    }

    throw QrException( QR_GRAPHICS_API_ERROR, "RasterizedDataCollector::PushInfo error" );
}

void RasterizedDataCollector::CopyFromArrayOfStructs(
    const QrRasterizedGeometryUploadInfo &info, ShVertex *dstVerts)
{
    assert(info.pVertices != nullptr);

    static_assert(std::is_same_v<decltype(info.pVertices), const QrVertex * >);
    static_assert(sizeof(ShVertex)                      == sizeof(QrVertex));
    static_assert(offsetof(ShVertex, position)          == offsetof(QrVertex, position));
    static_assert(offsetof(ShVertex, normal)            == offsetof(QrVertex, normal));
    static_assert(offsetof(ShVertex, texCoord)          == offsetof(QrVertex, texCoord));
    static_assert(offsetof(ShVertex, texCoordLayer1)    == offsetof(QrVertex, texCoordLayer1));
    static_assert(offsetof(ShVertex, texCoordLayer2)    == offsetof(QrVertex, texCoordLayer2));
    static_assert(offsetof(ShVertex, packedColor)       == offsetof(QrVertex, packedColor));

    memcpy(dstVerts, info.pVertices, sizeof(QrVertex) * info.vertexCount);
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
    vertexBuffer->CopyFromStaging(cmd, frameIndex, sizeof(QrVertex) * curVertexCount);
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
