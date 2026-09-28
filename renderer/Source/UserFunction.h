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

#include "qray/qray.h"

namespace qray
{

class UserPrint
{
public:
    UserPrint(PFN_qrPrint printFunc, void *pUserData);
    ~UserPrint() = default;

    UserPrint(const UserPrint &other) = delete;
    UserPrint(UserPrint &&other) noexcept = delete;
    UserPrint &operator=(const UserPrint &other) = delete;
    UserPrint &operator=(UserPrint &&other) noexcept = delete;

    void Print(const char *pMessage) const;

private:
    PFN_qrPrint printFunc;
    void *pUserData;
};



// A class to simplify calling qrOpenFile and qrCloseFile.
class UserFileLoad
{
    // This struct will automatically call qrCloseFile.
    // Must be contructed only by UserFileLoad::OpenFile.
    struct UserFileLoadHandle
    {
        ~UserFileLoadHandle();
        
        UserFileLoadHandle(UserFileLoadHandle &&other) noexcept;
        UserFileLoadHandle &operator=(UserFileLoadHandle &&other) noexcept;
        // restrict copying
        UserFileLoadHandle(const UserFileLoadHandle &other) = delete;
        UserFileLoadHandle &operator=(const UserFileLoadHandle &other) = delete;

        operator bool() const;
        bool Contains() const;

        const void *pData;
        uint32_t dataSize;

    private:
        friend class UserFileLoad;
        UserFileLoadHandle(const UserFileLoad *pUFL, const char *pFilePath);

    private:
        const UserFileLoad *pUFL;
        void *pFileHandle;
    };

public:
    UserFileLoad(PFN_qrOpenFile openFileFunc, PFN_qrCloseFile closeFileFunc, void *pUserData);
    ~UserFileLoad() = default;

    UserFileLoad(const UserFileLoad &other) = delete;
    UserFileLoad(UserFileLoad &&other) noexcept = delete;
    UserFileLoad &operator=(const UserFileLoad &other) = delete;
    UserFileLoad &operator=(UserFileLoad &&other) noexcept = delete;

    bool Exists() const;
    UserFileLoadHandle Open(const char *pFilePath) const;

    // These function should be called only by UserFileLoadHandle's constructor/destructor.
    void OpenFile(const char *pFilePath, const void **ppOutData, uint32_t *pOutDataSize, void **ppOutFileUserHandle) const;
    void CloseFile(void *pFileUserHandle) const;

private:
    PFN_qrOpenFile openFileFunc;
    PFN_qrCloseFile closeFileFunc;
    void *pUserData;
};

}
