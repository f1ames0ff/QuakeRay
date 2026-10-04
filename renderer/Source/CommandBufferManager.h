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

#pragma once

#include <vector>

#include "Common.h"
#include "Containers.h"
#include "Queues.h"

namespace qray
{

class CommandBufferManager
{
public:
    explicit CommandBufferManager(VkDevice device, std::shared_ptr<Queues> queues);
    ~CommandBufferManager();

    CommandBufferManager(const CommandBufferManager &other) = delete;
    CommandBufferManager(CommandBufferManager &&other) noexcept = delete;
    CommandBufferManager &operator=(const CommandBufferManager &other) = delete;
    CommandBufferManager &operator=(CommandBufferManager &&other) noexcept = delete;

    void PrepareForFrame(uint32_t frameIndex);

    VkCommandBuffer StartGraphicsCmd();
    VkCommandBuffer StartComputeCmd();
    VkCommandBuffer StartTransferCmd();

    void Submit(VkCommandBuffer cmd, VkFence fence = VK_NULL_HANDLE);
    void Submit(VkCommandBuffer cmd, VkSemaphore waitSemaphore, VkPipelineStageFlags waitStages, VkSemaphore signalSemaphore, VkFence fence);

    void WaitGraphicsIdle();
    void WaitComputeIdle();
    void WaitTransferIdle();
    void WaitDeviceIdle();

private:
    struct AllocatedCmds
    {
        std::vector<VkCommandBuffer> cmds = {};
        uint32_t curCount = 0;
        VkCommandPool pool = VK_NULL_HANDLE;
    };

private:
    VkCommandBuffer StartCmd(uint32_t frameIndex, AllocatedCmds &cmds, VkQueue queue);

private:
    VkDevice device;

    uint32_t currentFrameIndex;

    const uint32_t cmdAllocStep = 16;

    AllocatedCmds graphicsCmds[MAX_FRAMES_IN_FLIGHT];
    AllocatedCmds computeCmds[MAX_FRAMES_IN_FLIGHT];
    AllocatedCmds transferCmds[MAX_FRAMES_IN_FLIGHT];

    std::weak_ptr<Queues> queues;
    rgl::unordered_map<VkCommandBuffer, VkQueue> cmdQueues[MAX_FRAMES_IN_FLIGHT];
};

}
