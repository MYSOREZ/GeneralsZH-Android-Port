/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GeneralsX @feature Codex/realkot 16/10/2026 Exact values in the Extras menu (ExtrasMenu.cpp), from
// https://github.com/realkot/GeneralsX, used with the author's permission.
#pragma once
#include <algorithm>

// GeneralsX @feature Codex 16/10/2026 Validate exact Extras input before touching preferences.
namespace GXExtrasValues {
template<class Char> inline bool digits(const Char *text,int length,int &value) {
    if(!text || length<=0 || length>6)return false;
    int result=0;
    for(int i=0;i<length;++i) {
        if(text[i]<'0'||text[i]>'9')return false;
        result=result*10+(text[i]-'0');
    }
    value=result;return true;
}
// GeneralsX @bugfix Android port 18/10/2026 GameData's MaxCameraHeight before the Extras menu first
// overrode it this session (0 until then): the main menu's scene keeps the retail height
// (W3DView::setZoomToMax).
inline float &retailMaxCameraHeight() {
    static float value=0.0f;
    return value;
}
inline int normalize(int value,int low,int high,int quantum=1) {
    value=std::max(low,std::min(high,value));
    return std::max(low,std::min(high,value/quantum*quantum));
}
}
