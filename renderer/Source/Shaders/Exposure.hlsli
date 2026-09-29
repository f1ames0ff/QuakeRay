// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>
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



#ifndef EXPOSURE_HLSLI_
#define EXPOSURE_HLSLI_
#ifndef DESC_SET_TONEMAPPING
    #error DESC_SET_TONEMAPPING is required
#endif

float getManualEV100( float aperture, float shutterTime, float iso )
{
    return log2( square( aperture ) / shutterTime * 100.0 / iso );
}

float getAutoEV100()
{
    const float lumAverage = max( 0.0, tonemapping[0].avgLuminance );
    const float S          = 100;
    const float K          = 12.5;
    return log2( lumAverage * S / K );
}

float getCurrentEV100()
{
    bool manual = false;
    return manual ? getManualEV100( 1.0 / 16.0, 1.0 / 125.0, 100 ) : getAutoEV100();
}

float ev100ToLuminousExposure( float ev100 )
{
    float maxLuminance = 1.2 * exp2( ev100 );
    return maxLuminance > 0.0 ? 1.0 / maxLuminance : 0.0;
}

float ev100ToLuminance( float ev100 )
{
    return exp2( ev100 - 3 );
}

#endif
