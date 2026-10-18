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

// GeneralsX @feature Codex/realkot 15/10/2026 Where the Tactical scheme's army column goes (GroupPanel.cpp).
// Ported from https://github.com/realkot/GeneralsX with the author's permission.
#pragma once
#include <algorithm>

// GeneralsX @feature Codex 15/10/2026 Pure screen-space policy shared by runtime and QA.
// A wide screen puts the army column below the HUD's top, in its left gap. When that
// gap is too narrow, keep readable buttons above the HUD rather than cover its controls.
namespace GXArmyPanelLayout {
struct Layout {
    int x, y, width, height, gap;
    int rowX, rowY, cellWidth, cellHeight, columns, rows;
};
inline Layout calculate(int width, int height, int barLeft, int barTop,
                        int safeLeft, int safeBottom, float fontScale) {
    const float resolution = std::max(0.25f, std::min(width/800.0f, height/600.0f));
    const float unit = resolution * std::max(1.0f, fontScale);
    const int margin = std::max(4, (int)(6*resolution));
    const int gap = std::max(2, (int)(4*resolution));
    const int available = barLeft - safeLeft - 2*margin;
    const int bottom = std::max(1, height - safeBottom - margin);
    const int gapHeight = bottom - barTop;
    const bool beside = available >= (int)(64*unit) && gapHeight >= (int)(120*unit) + 5*gap;
    Layout p{};
    p.x = safeLeft + margin;
    p.gap = gap;
    p.width = std::max(1, std::min((int)(80*unit), beside ? available : width-p.x-margin));
    const int space = beside ? gapHeight : std::max(1, barTop-margin);
    p.height = std::max(1, std::min((int)(34*unit), (space-5*gap)/6));
    p.y = std::max(margin, (beside ? bottom : barTop-margin) - 6*p.height-5*gap);
    p.columns = beside ? 2 : 5;
    p.rows = 10 / p.columns;
    p.cellWidth = beside ? std::max(1,(p.width-gap)/2) : std::max(1,std::min((int)(36*unit),(width-p.x-margin-4*gap)/5));
    p.cellHeight = beside ? std::max(1,std::min(p.height,(p.y-margin-(p.rows-1)*gap)/p.rows)) : p.height;
    p.rowX = beside ? p.x : p.x+p.width+gap;
    if (!beside) p.cellWidth=std::max(1,std::min((int)(36*unit),(width-p.rowX-margin-4*gap)/5));
    p.rowY = beside ? std::max(0,p.y-gap-p.rows*p.cellHeight-(p.rows-1)*gap) : p.y;
    return p;
}
}
