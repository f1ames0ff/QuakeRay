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

#include "UserFunction.h"

#include <cassert>
#include <utility>

qray::UserPrint::UserPrint(PFN_qrPrint _printFunc, void *_pUserData)
    : printFunc(_printFunc), pUserData(_pUserData) {}

void qray::UserPrint::Print(const char *pMessage) const
{
    if (printFunc != nullptr)
    {
        printFunc(pMessage, pUserData);
    }
}

qray::UserFileLoad::UserFileLoadHandle::UserFileLoadHandle(const UserFileLoad *_pUFL, const char *_pFilePath)
    : pData(nullptr), dataSize(0), pUFL(_pUFL), pFileHandle(nullptr)
{
    assert(pUFL != nullptr);
    pUFL->OpenFile(_pFilePath, &pData, &dataSize, &pFileHandle);
}

qray::UserFileLoad::UserFileLoadHandle::~UserFileLoadHandle()
{
    assert(pUFL != nullptr);
    pUFL->CloseFile(pFileHandle);
}

qray::UserFileLoad::UserFileLoadHandle::UserFileLoadHandle(UserFileLoadHandle &&other) noexcept
    : pData(std::exchange(other.pData, nullptr))
    , dataSize(std::exchange(other.dataSize, 0))
    , pUFL(std::exchange(other.pUFL, nullptr))
    , pFileHandle(std::exchange(other.pFileHandle, nullptr))
{
}

qray::UserFileLoad::UserFileLoadHandle &qray::UserFileLoad::UserFileLoadHandle::operator=(UserFileLoadHandle &&other) noexcept
{
    pData = std::exchange(other.pData, nullptr);
    dataSize = std::exchange(other.dataSize, 0);
    pUFL = std::exchange(other.pUFL, nullptr);
    pFileHandle = std::exchange(other.pFileHandle, nullptr);

    return *this;
}

qray::UserFileLoad::UserFileLoadHandle::operator bool() const
{
    return Contains();
}

bool qray::UserFileLoad::UserFileLoadHandle::Contains() const
{
    return pData != nullptr && dataSize > 0;
}

qray::UserFileLoad::UserFileLoad(PFN_qrOpenFile _openFileFunc, PFN_qrCloseFile _closeFileFunc, void *_pUserData)
    : openFileFunc(_openFileFunc), closeFileFunc(_closeFileFunc), pUserData(_pUserData) {}

bool qray::UserFileLoad::Exists() const
{
    return openFileFunc != nullptr && closeFileFunc != nullptr;
}

qray::UserFileLoad::UserFileLoadHandle qray::UserFileLoad::Open(const char *pFilePath) const
{
    return UserFileLoadHandle(this, pFilePath);
}

void qray::UserFileLoad::OpenFile(const char *pFilePath, const void **ppOutData, uint32_t *pOutDataSize, void **ppOutFileUserHandle) const
{
    *ppOutData = nullptr;
    *pOutDataSize = 0;

    if (Exists())
    {
        openFileFunc(pFilePath, pUserData, ppOutData, pOutDataSize, ppOutFileUserHandle);
    }
}

void qray::UserFileLoad::CloseFile(void *pFileUserHandle) const
{
    if (Exists())
    {
        closeFileFunc(pFileUserHandle, pUserData);
    }
}
