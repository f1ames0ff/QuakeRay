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

#pragma once

#include <vector>

#include <vkpt/vkpt.h>
#include "AutoBuffer.h"
#include "Common.h"
#include "TextureManager.h"
#include "Utils.h"

namespace vkpt
{
    struct ShVertex;

    class RasterizedDataCollector final
    {
    public:
        struct DrawInfo
        {
            RgTransform                 transform = {};
            std::optional< Float16D >   viewProj  = std::nullopt;
            std::optional< VkViewport > viewport  = std::nullopt;

            uint32_t vertexCount = 0;
            uint32_t firstVertex = 0;
            uint32_t indexCount  = 0;
            uint32_t firstIndex  = 0;

            Float4D  color                = Float4D( NullifyToken );
            uint32_t textureIndex         = 0;
            uint32_t emissionTextureIndex = 0;

            RgRasterizedGeometryStateFlags pipelineState = 0;
            RgBlendFactor                  blendFuncSrc  = RG_BLEND_FACTOR_ONE;
            RgBlendFactor                  blendFuncDst  = RG_BLEND_FACTOR_ONE;

            Float4D  smokeNoise = Float4D( NullifyToken );
            Float4D  smokeLook  = Float4D( NullifyToken );
        };

    public:
        explicit RasterizedDataCollector( VkDevice                            device,
                                          std::shared_ptr< MemoryAllocator >& allocator,
                                          std::shared_ptr< TextureManager >   textureMgr,
                                          uint32_t                            maxVertexCount,
                                          uint32_t                            maxIndexCount );
        ~RasterizedDataCollector();

        RasterizedDataCollector( const RasterizedDataCollector& other )     = delete;
        RasterizedDataCollector( RasterizedDataCollector&& other ) noexcept = delete;
        RasterizedDataCollector& operator=( const RasterizedDataCollector& other ) = delete;

        RasterizedDataCollector& operator=( RasterizedDataCollector&& other ) noexcept = delete;
        void                     AddGeometry( uint32_t                              frameIndex,
                                              const RgRasterizedGeometryUploadInfo& info,
                                              const float*                          viewProjection,
                                              const RgViewport*                     viewport );

        void Clear( uint32_t frameIndex );

        void CopyFromStaging( VkCommandBuffer cmd, uint32_t frameIndex );

        VkBuffer GetVertexBuffer() const;
        VkBuffer GetIndexBuffer() const;

        VkBuffer GetVertexStagingBuffer(uint32_t frameIndex);
        VkBuffer GetIndexStagingBuffer(uint32_t frameIndex);

        VkDeviceSize GetVertexBufferSize() const;
        VkDeviceSize GetIndexBufferSize() const;

        static uint32_t GetVertexStride();
        static void     GetVertexLayout( VkVertexInputAttributeDescription* outAttrs,
                                         uint32_t*                          outAttrsCount );
        static void     GetSmokeVertexLayout( VkVertexInputAttributeDescription* outAttrs,
                                              uint32_t*                          outAttrsCount );

        const std::vector< DrawInfo >& GetRasterDrawInfos() const;
        const std::vector< DrawInfo >& GetSwapchainDrawInfos() const;
        const std::vector< DrawInfo >& GetSkyDrawInfos() const;

    protected:
        DrawInfo& PushInfo( RgRasterizedGeometryRenderType renderType );

    private:
        static void CopyFromArrayOfStructs( const RgRasterizedGeometryUploadInfo& info,
                                            ShVertex*                             dstVerts );

    private:
        VkDevice                          device;
        std::shared_ptr< TextureManager > textureMgr;

        std::shared_ptr< AutoBuffer > vertexBuffer;
        std::shared_ptr< AutoBuffer > indexBuffer;

        uint64_t curVertexCount;
        uint64_t curIndexCount;

        std::vector< DrawInfo > rasterDrawInfos;
        std::vector< DrawInfo > swapchainDrawInfos;
        std::vector< DrawInfo > skyDrawInfos;
    };

}
