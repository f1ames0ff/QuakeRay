#pragma once

#include "../Generated/ShaderCommonC.h"
#include <nvrhi/nvrhi.h>
#include <cstddef>

namespace qray::rhi
{

inline void transferExposureHistory(nvrhi::ICommandList *commandList,
                                    nvrhi::IBuffer *previous, nvrhi::IBuffer *current)
{
    if (previous == current)
    {
        return;
    }

    commandList->copyBuffer(current, offsetof(ShTonemapping, curve),
                            previous, offsetof(ShTonemapping, curve), sizeof(ShTonemapping::curve));
    commandList->copyBuffer(current, offsetof(ShTonemapping, adaptedLuminance),
                            previous, offsetof(ShTonemapping, adaptedLuminance), sizeof(float) * 2);
    commandList->setBufferState(previous, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(current, nvrhi::ResourceStates::UnorderedAccess);
}

}
