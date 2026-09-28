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

#include "UserFunction.h"

#include <cassert>
#include <utility>

vkpt::UserPrint::UserPrint(PFN_rgPrint _printFunc, void *_pUserData)
    : printFunc(_printFunc), pUserData(_pUserData) {}

void vkpt::UserPrint::Print(const char *pMessage) const
{
    if (printFunc != nullptr)
    {
        printFunc(pMessage, pUserData);
    }
}

vkpt::UserFileLoad::UserFileLoadHandle::UserFileLoadHandle(const UserFileLoad *_pUFL, const char *_pFilePath)
    : pData(nullptr), dataSize(0), pUFL(_pUFL), pFileHandle(nullptr)
{
    assert(pUFL != nullptr);
    pUFL->OpenFile(_pFilePath, &pData, &dataSize, &pFileHandle);
}

vkpt::UserFileLoad::UserFileLoadHandle::~UserFileLoadHandle()
{
    assert(pUFL != nullptr);
    pUFL->CloseFile(pFileHandle);
}

vkpt::UserFileLoad::UserFileLoadHandle::UserFileLoadHandle(UserFileLoadHandle &&other) noexcept
    : pData(std::exchange(other.pData, nullptr))
    , dataSize(std::exchange(other.dataSize, 0))
    , pUFL(std::exchange(other.pUFL, nullptr))
    , pFileHandle(std::exchange(other.pFileHandle, nullptr))
{
}

vkpt::UserFileLoad::UserFileLoadHandle &vkpt::UserFileLoad::UserFileLoadHandle::operator=(UserFileLoadHandle &&other) noexcept
{
    pData = std::exchange(other.pData, nullptr);
    dataSize = std::exchange(other.dataSize, 0);
    pUFL = std::exchange(other.pUFL, nullptr);
    pFileHandle = std::exchange(other.pFileHandle, nullptr);

    return *this;
}

vkpt::UserFileLoad::UserFileLoadHandle::operator bool() const
{
    return Contains();
}

bool vkpt::UserFileLoad::UserFileLoadHandle::Contains() const
{
    return pData != nullptr && dataSize > 0;
}

vkpt::UserFileLoad::UserFileLoad(PFN_rgOpenFile _openFileFunc, PFN_rgCloseFile _closeFileFunc, void *_pUserData)
    : openFileFunc(_openFileFunc), closeFileFunc(_closeFileFunc), pUserData(_pUserData) {}

bool vkpt::UserFileLoad::Exists() const
{
    return openFileFunc != nullptr && closeFileFunc != nullptr;
}

vkpt::UserFileLoad::UserFileLoadHandle vkpt::UserFileLoad::Open(const char *pFilePath) const
{
    return UserFileLoadHandle(this, pFilePath);
}

void vkpt::UserFileLoad::OpenFile(const char *pFilePath, const void **ppOutData, uint32_t *pOutDataSize, void **ppOutFileUserHandle) const
{
    *ppOutData = nullptr;
    *pOutDataSize = 0;

    if (Exists())
    {
        openFileFunc(pFilePath, pUserData, ppOutData, pOutDataSize, ppOutFileUserHandle);
    }
}

void vkpt::UserFileLoad::CloseFile(void *pFileUserHandle) const
{
    if (Exists())
    {
        closeFileFunc(pFileUserHandle, pUserData);
    }
}
