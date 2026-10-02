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

#include "CommandBufferManager.h"

using namespace qray;

CommandBufferManager::CommandBufferManager(VkDevice _device, std::shared_ptr<Queues> _queues)
    : device(_device)
    , currentFrameIndex(MAX_FRAMES_IN_FLIGHT - 1)
    , queues(_queues)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = 0;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        poolInfo.queueFamilyIndex = _queues->GetIndexGraphics();
        VK_CHECKERROR(vkCreateCommandPool(device, &poolInfo, nullptr, &graphicsCmds[i].pool));

        poolInfo.queueFamilyIndex = _queues->GetIndexCompute();
        VK_CHECKERROR(vkCreateCommandPool(device, &poolInfo, nullptr, &computeCmds[i].pool));

        poolInfo.queueFamilyIndex = _queues->GetIndexTransfer();
        VK_CHECKERROR(vkCreateCommandPool(device, &poolInfo, nullptr, &transferCmds[i].pool));
    }
}

CommandBufferManager::~CommandBufferManager()
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        assert(cmdQueues[i].empty());

        vkDestroyCommandPool(device, graphicsCmds[i].pool, nullptr);
        vkDestroyCommandPool(device, computeCmds[i].pool, nullptr);
        vkDestroyCommandPool(device, transferCmds[i].pool, nullptr);
    }
}

void CommandBufferManager::PrepareForFrame(uint32_t frameIndex)
{
    assert(cmdQueues[frameIndex].empty());

    const auto resetPool = [this](AllocatedCmds &allocated)
    {
        if (allocated.curCount > 0)
        {
            vkResetCommandPool(device, allocated.pool, 0);
            allocated.curCount = 0;
        }
    };

    resetPool(graphicsCmds[frameIndex]);
    resetPool(computeCmds[frameIndex]);
    resetPool(transferCmds[frameIndex]);

    currentFrameIndex = frameIndex;
}

VkCommandBuffer CommandBufferManager::StartCmd(uint32_t frameIndex, AllocatedCmds &allocated, VkQueue queue)
{
    if (allocated.curCount >= allocated.cmds.size())
    {
        const uint32_t oldCount = static_cast<uint32_t>(allocated.cmds.size());
        allocated.cmds.resize(oldCount + cmdAllocStep);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = allocated.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = cmdAllocStep;

        VK_CHECKERROR(vkAllocateCommandBuffers(device, &allocInfo, &allocated.cmds[oldCount]));
    }

    const VkCommandBuffer cmd = allocated.cmds[allocated.curCount++];

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VK_CHECKERROR(vkBeginCommandBuffer(cmd, &beginInfo));

    cmdQueues[frameIndex][cmd] = queue;

    return cmd;
}

VkCommandBuffer CommandBufferManager::StartGraphicsCmd()
{
    const std::shared_ptr<Queues> aliveQueues = queues.lock();

    if (!aliveQueues)
    {
        return VK_NULL_HANDLE;
    }

    return StartCmd(currentFrameIndex, graphicsCmds[currentFrameIndex], aliveQueues->GetGraphics());
}

VkCommandBuffer CommandBufferManager::StartComputeCmd()
{
    const std::shared_ptr<Queues> aliveQueues = queues.lock();

    if (!aliveQueues)
    {
        return VK_NULL_HANDLE;
    }

    return StartCmd(currentFrameIndex, computeCmds[currentFrameIndex], aliveQueues->GetCompute());
}

VkCommandBuffer CommandBufferManager::StartTransferCmd()
{
    const std::shared_ptr<Queues> aliveQueues = queues.lock();

    if (!aliveQueues)
    {
        return VK_NULL_HANDLE;
    }

    return StartCmd(currentFrameIndex, transferCmds[currentFrameIndex], aliveQueues->GetTransfer());
}

void CommandBufferManager::Submit(VkCommandBuffer cmd, VkFence fence)
{
    VK_CHECKERROR(vkEndCommandBuffer(cmd));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    auto &frameQueues = cmdQueues[currentFrameIndex];
    assert(frameQueues.find(cmd) != frameQueues.end());

    const VkQueue queue = frameQueues[cmd];
    frameQueues.erase(cmd);

    VK_CHECKERROR(vkQueueSubmit(queue, 1, &submitInfo, fence));
}

void CommandBufferManager::Submit(VkCommandBuffer cmd, VkSemaphore waitSemaphore, VkPipelineStageFlags waitStages,
    VkSemaphore signalSemaphore, VkFence fence)
{
    VK_CHECKERROR(vkEndCommandBuffer(cmd));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &waitSemaphore;
    submitInfo.pWaitDstStageMask = &waitStages;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &signalSemaphore;

    auto &frameQueues = cmdQueues[currentFrameIndex];
    assert(frameQueues.find(cmd) != frameQueues.end());

    const VkQueue queue = frameQueues[cmd];
    frameQueues.erase(cmd);

    VK_CHECKERROR(vkQueueSubmit(queue, 1, &submitInfo, fence));
}

void CommandBufferManager::WaitGraphicsIdle()
{
    if (const std::shared_ptr<Queues> aliveQueues = queues.lock())
    {
        VK_CHECKERROR(vkQueueWaitIdle(aliveQueues->GetGraphics()));
    }
}

void CommandBufferManager::WaitComputeIdle()
{
    if (const std::shared_ptr<Queues> aliveQueues = queues.lock())
    {
        VK_CHECKERROR(vkQueueWaitIdle(aliveQueues->GetCompute()));
    }
}

void CommandBufferManager::WaitTransferIdle()
{
    if (const std::shared_ptr<Queues> aliveQueues = queues.lock())
    {
        VK_CHECKERROR(vkQueueWaitIdle(aliveQueues->GetTransfer()));
    }
}

void CommandBufferManager::WaitDeviceIdle()
{
    vkDeviceWaitIdle(device);
}
