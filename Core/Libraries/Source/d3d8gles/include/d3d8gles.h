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

/*
** d3d8gles.h - public entry points for the native GLES3 D3D8 backend.
**
** GeneralsX @build Android port GLES experiment - ported from
** Lolendor/Generals-WebAssembly's d3d8webgl (D3D8 -> WebGL2), adapted to
** native GLES3 via SDL3 instead of Emscripten/WebGL2. See
** Core/Libraries/Source/d3d8gles/src/d3d8gles.cpp for the implementation.
*/

#pragma once

#include <d3d8.h>

// Statically linked, unlike DXVK's Direct3DCreate8 which is dlopen'd from
// libdxvk_d3d8.so -- named distinctly so both can coexist in libmain.so.
extern "C" IDirect3D8 *WINAPI Direct3DCreate8_GLES(UINT sdkVersion);

// Called from the SDL3 window-resize path so the GLES pipeline's cached
// framebuffer size stays in sync without waiting for the next Reset().
extern "C" void d3d8gles_resize(int w, int h);

// GeneralsX @build Android port GLES experiment - GPU instancing for
// repeated draws of identical geometry (see DX8PolygonRendererClass::
// Render_Instanced() / DX8Wrapper::Draw_Triangles_Instanced() in WW3D2,
// which call this). Real D3D8 has no instancing concept to extend, and
// IDirect3DDevice8 (references/fbraz3-dxvk's vendored d3d8.h) must not be
// touched -- this free-function escape hatch is the established pattern
// for GLES-only entry points, same as Direct3DCreate8_GLES/d3d8gles_resize
// above. worldMatrices is instanceCount consecutive 16-float blocks, each
// already in the same row-major-uploaded-untransposed layout
// DX8Wrapper::Set_Transform's non-instanced path feeds glUniformMatrix4fv
// today (see WebGLPipeline::drawIndexedInstanced's comment) -- callers must
// not reformat it.
extern "C" void d3d8gles_drawIndexedInstanced(
	IDirect3DDevice8 *dev, D3DPRIMITIVETYPE primType,
	UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount,
	const float *worldMatrices, int instanceCount);
