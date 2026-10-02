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

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace qray::LibraryConfig
{
    struct Config
    {
        bool vulkanValidation = false;
        bool developerMode = false;
        bool dlssValidation = false;
        bool fpsMonitor = false;
    };

    namespace detail
    {
        struct Entry
        {
            std::string_view    name;
            bool Config::*      setting;
        };

        inline constexpr Entry ENTRIES[] =
        {
            { "vulkanvalidation", &Config::vulkanValidation },
            { "developer",        &Config::developerMode    },
            { "dlssvalidation",   &Config::dlssValidation   },
            { "fpsmonitor",       &Config::fpsMonitor       },
        };

        inline void ProcessEntry(Config &dst, std::string_view entry)
        {
            for (const Entry &e : ENTRIES)
            {
                if (e.name == entry)
                {
                    dst.*(e.setting) = true;
                    break;
                }
            }
        }
    }

    inline Config Read(const char *pPath)
    {
        if (pPath == nullptr || pPath[0] == '\0')
        {
            pPath = "qray.txt";
        }

        const std::filesystem::path path(pPath);

        if (!std::filesystem::exists(path))
        {
            return {};
        }

        std::ifstream file(path);

        if (!file.is_open())
        {
            return {};
        }

        Config result = {};

        for (std::string line; std::getline(file, line); )
        {
            std::ranges::transform(line, line.begin(), ::tolower);

            detail::ProcessEntry(result, line);
        }

        return result;
    }
}
