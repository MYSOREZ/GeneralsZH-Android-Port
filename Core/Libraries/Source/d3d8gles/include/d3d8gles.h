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

// GeneralsX @build Android port render-backend picker 07/09/2026 - single
// source of truth for the Vulkan/GLES/GLES+ANGLE choice, called from every
// place that needs to agree on it: SDL3Main.cpp (decides which kind of SDL
// window/EGL surface to create) and dx8wrapper.cpp (decides whether to load
// libdxvk_d3d8.so or use Direct3DCreate8_GLES). These USED to be two
// separate copies of the same getenv() check; they drifted out of sync the
// moment the Setup app's render_backend.cfg picker was added to only one of
// them (SDL3Main.cpp), so a phone with "Vulkan" selected got a Vulkan SDL
// window from SDL3Main.cpp but dx8wrapper.cpp still silently loaded the GLES
// backend underneath it -- SDL_GL_CreateContext then failed ("the specified
// window isn't an OpenGL window") and nothing ever rendered, while the rest
// of the engine (audio, game logic) ran fine. Implemented in d3d8gles.cpp
// (which already links sdl3lib) so both callers -- one of which, WW3D2, does
// NOT itself link sdl3lib -- can share one implementation instead of each
// re-reading render_backend.cfg/the env vars themselves.
extern "C" bool d3d8gles_ShouldUseVulkanBackend();
extern "C" bool d3d8gles_ShouldUseANGLE();
