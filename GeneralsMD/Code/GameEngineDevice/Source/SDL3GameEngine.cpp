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
** SDL3GameEngine.cpp
**
** Linux implementation of GameEngine using SDL3 for windowing/input.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Provides SDL3-based input and window management for Linux builds.
** Based on fighter19 reference implementation.
*/

#ifndef _WIN32

#include "SDL3GameEngine.h"
#include "OpenALAudioManager.h"
#include "SDL3Device/GameClient/SDL3Mouse.h"
#include "SDL3Device/GameClient/SDL3Keyboard.h"
#include "Common/MessageStream.h"
#include "GameClient/Mouse.h"
#include "GameClient/Keyboard.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/View.h"
#include "GameClient/Shell.h"
#include "GameClient/InGameUI.h"
#include "W3DDevice/GameLogic/W3DGameLogic.h"
#include "W3DDevice/GameClient/W3DGameClient.h"
#include "W3DDevice/Common/W3DModuleFactory.h"
#include "W3DDevice/Common/W3DThingFactory.h"
#include "W3DDevice/Common/W3DFunctionLexicon.h"
#include "W3DDevice/Common/W3DRadar.h"
#include "W3DDevice/GameClient/W3DParticleSys.h"
#include "W3DDevice/GameClient/W3DWebBrowser.h"
#include "StdDevice/Common/StdLocalFileSystem.h"
#include "StdDevice/Common/StdBIGFileSystem.h"
#include "Common/GlobalData.h"
#include "GXTrace.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// GeneralsX @build Android port 06/07/2026 Shared guard for the touch-first
// mobile platforms. The gesture translator and app-lifecycle render gate below
// were built for iOS and apply 1:1 on Android: both OSes deliver SDL finger
// events, both suspend the process when the app leaves the foreground, and on
// both the window surface is owned by the OS while backgrounded (CAMetalLayer
// on iOS, ANativeWindow on Android) — touching the GPU in that state kills the
// app on resume.
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
#define SAGE_MOBILE_PLATFORM 1
#endif

// Extern globals for input devices (set by GameClient)
extern Mouse *TheMouse;
extern Keyboard *TheKeyboard;
extern GameWindowManager *TheWindowManager;

#if defined(SAGE_MOBILE_PLATFORM)
#include <atomic>

// ---------------------------------------------------------------------------
// Mobile (iOS/Android) app lifecycle
//
// iOS and Android suspend the process when the app leaves the foreground. Any
// GPU work submitted around suspension stalls on drawable acquisition (MoltenVK
// waits out a timeout per present; Android tears down the ANativeWindow under
// the swapchain), which surfaces as multi-second input hangs right
// after resuming. SDL warns that lifecycle events can arrive outside the
// normal poll cycle, so they are captured in an event watcher that fires
// immediately on the delivering thread; the engine update loop checks the
// flag and skips simulation + rendering while backgrounded.
// ---------------------------------------------------------------------------
// Two independent reasons to halt the render/sim loop:
//  - BACKGROUNDED (home / switched away): the process is about to be suspended.
//  - INACTIVE (multitasking switcher open, Control Center, a notification
//    banner): iOS snapshots the window and owns the CAMetalLayer drawable during
//    this window — and crucially, opening the app switcher fires resign-active
//    WITHOUT a full background transition.
// Acquiring a Metal drawable during EITHER state fights iOS for the layer; across
// repeated suspend/switcher cycles MoltenVK is driven into an unrecoverable
// surface state and the app crashes (the reported "crashes after backgrounding /
// multitasking a few times"). Pause whenever either is set.
static std::atomic<bool> s_appBackgrounded{false};
static std::atomic<bool> s_appInactive{false};

static inline bool mobileShouldPauseRendering()
{
	return s_appBackgrounded.load() || s_appInactive.load();
}

static bool SDLCALL mobileLifecycleWatcher(void *userdata, SDL_Event *event)
{
	switch (event->type) {
		case SDL_EVENT_WILL_ENTER_BACKGROUND:
		case SDL_EVENT_DID_ENTER_BACKGROUND:
			s_appBackgrounded.store(true);
			break;
		case SDL_EVENT_DID_ENTER_FOREGROUND:
			s_appBackgrounded.store(false);
			break;
		// Resign/become active. On iOS, SDL maps applicationWillResignActive ->
		// window focus lost and applicationDidBecomeActive -> window focus gained.
		// Stay paused until fully active again (focus regained), which arrives
		// after DID_ENTER_FOREGROUND.
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			s_appInactive.store(true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			s_appInactive.store(false);
			break;
		default:
			break;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Touch input (iOS + Android)
//
// SDL's automatic touch-mouse synthesis is disabled on mobile (SDL3Main.cpp
// sets SDL_HINT_TOUCH_MOUSE_EVENTS=0) -- this code owns touch interpretation
// completely.
//
// GeneralsX @feature Android port 01/08/2026, rewritten 01/08/2026 twice
// Native touch control, with NO mouse hardware emulation anywhere:
//
// - Camera pan/zoom call TheTacticalView->userScrollBy()/userZoom()
//   directly (applyCameraPan()/applyCameraZoom()), driven by real per-frame
//   finger deltas. Nothing mouse-shaped is involved at all.
//
// - Selection, GUI clicks, right-click commands, and selection-box/slider
//   drags go straight onto TheMessageStream as the same MSG_RAW_MOUSE_*
//   GameMessages a real mouse would produce (pushMousePosition()/
//   pushMouseButton() below) -- but pushed directly from
//   real finger events, not synthesized as SDL_Event mouse structs fed
//   through SDL3Mouse's hardware-mouse pipeline. This is as close to "no
//   mouse system" as this engine's input architecture allows: selection
//   (SelectionXlat.cpp), GUI hover/hilite (WindowXlat.cpp), and the
//   multiplayer-sync message stream itself are all keyed on that exact
//   GameMessage vocabulary -- built in 2003 for a PC mouse, and there is no
//   bypassing that queue for ANY input device without rewriting those
//   systems (out of scope; this is the original engine's synchronization
//   mechanism for multiplayer/replays, not a design choice we get to
//   route around). What we DO get to remove is everything downstream of
//   that: no persisted cursor object, no OS mouse cursor, no hover-then-
//   click timing built for a physical mouse, no per-frame position
//   heartbeat -- SDL3Mouse::createStreamMessages() is a no-op on mobile
//   (see SDL3Mouse.cpp) specifically so nothing but real touch events ever
//   produces a mouse-shaped message. A GameMessage type being literally
//   named MSG_RAW_MOUSE_LEFT_BUTTON_DOWN is a historical label, not mouse
//   emulation: no mouse is being pretended to exist, no cursor persists
//   between touches, and nothing here reads back "where the mouse is".
//
// Gestures, deliberately as few and as standard as an Android map/RTS app
// gets:
//   1 finger tap                       -> select / issue command (LMB click)
//   1 finger double-tap                -> select all of that unit's type on
//                                          screen (existing double-click handling)
//   1 finger drag, at ANY point, however long the finger sat still first ->
//                                          direct camera pan, immediately
//   1 finger drag released while still moving fast -> pan keeps coasting,
//                                          decelerating (momentum/inertia),
//                                          until it dies down or another
//                                          finger touches down
//   1 finger held still the whole time, released without ever moving ->
//                                          right-click (issue command)
//   2 fingers                          -> direct camera pan (centroid delta)
//                                          AND zoom (spread delta) together,
//                                          once per frame, unconditionally
//   2 fingers tapped together, both staying near where they landed ->
//                                          right-click (fast "cancel selection")
//
// GeneralsX @bugfix Android port 01/08/2026, again Movement now ALWAYS wins
// over hold-duration, no matter when it happens. The previous cut fired the
// long-press right-click proactively the instant LONG_PRESS_MS elapsed
// (600ms) while the finger was still down, and any further drag from there
// upgraded to a selection-box drag instead of a pan -- so a completely
// ordinary drag that started with as little as half a second's hesitation
// (touch down, get oriented, THEN start moving -- unremarkable human
// timing) silently turned into "select a box of units" instead of "pan the
// camera", with no camera movement and no obvious visual cue why. Reported
// as "I drag with one finger like every other Android RTS and the camera
// just doesn't move, I'm stuck standing at my base". Fixed by resolving the
// long-press ONLY at release: if the finger crosses the pan dead zone at
// ANY time before lifting, however long it sat still first, it's a pan --
// long-press right-click only fires if the finger never moved at all,
// checked once, when it lifts. This removes single-finger press-and-hold-
// then-drag box-select entirely (it's what directly conflicted with "any
// drag pans") -- if box-select turns out to still be wanted, it needs its
// own separate gesture, not sharing single-finger drag with panning.
//
// GeneralsX @feature Android port 01/08/2026, again Two-finger tap-to-
// cancel is back: dropped in an earlier pass as "redundant with the
// single-finger long-press", but it's faster and more natural for
// "cancel/deselect" than reaching for a long-press, and was asked back
// explicitly. This does NOT reintroduce the old pan-vs-zoom classifier --
// TWOFINGER still always applies both pan and zoom every frame,
// unconditionally. This only ADDS a check at release: if neither finger
// moved more than TWO_FINGER_TAP_MAX_PX from where it landed, the whole
// gesture is also a tap -> fire right-click at the landing centroid.
//
// GeneralsX @bugfix Android port 01/08/2026 Previous revisions tried to
// classify a two-finger gesture as EITHER pan OR zoom (a "one finger
// anchored, the other drags vertically" heuristic with a commit distance,
// a displacement ratio, and a co-directionality check) before applying
// anything -- built to fix an OLDER architecture where zoom was a discrete
// wheel-tick and any accidental pan alongside it was jarring. That
// architecture is gone: pan and zoom are now both continuous, real-pixel,
// 1:1 camera control, so feeding both signals every frame -- exactly the way
// a normal two-finger touch UI (map apps, browsers, Rusted Warfare) already
// works -- is simply correct. A little pan drift from two hands never moving
// in perfect lockstep during a pinch is imperceptible next to the zoom
// itself; the classifier was solving a problem this architecture doesn't
// have, and its heuristics were exactly why zoom worked "sometimes" --
// real symmetric two-hand pinches (both fingers moving, neither "anchored")
// routinely failed the ratio check and were misclassified as pan.
//
// GeneralsX @bugfix Android port 01/08/2026 A prior revision added a fixed
// settle window (re-anchoring the pan reference point for the first 100ms
// after touch-down) to keep ordinary hold-still tremor from hijacking a tap
// into a pan. That timer could also silently eat an entire quick, deliberate
// flick if the whole gesture finished inside the window -- reported as
// "can't pan with my finger at all". Removed in favor of the simplest
// standard approach: a single dead-zone distance (TAP_DEAD_ZONE_PX) decides
// tap vs. drag, no timer involved, matching how every ordinary Android touch
// UI (not just this engine) already disambiguates the two.
// ---------------------------------------------------------------------------
namespace {

struct TouchState {
	enum Phase {
		IDLE,        // no fingers tracked
		PENDING,     // finger1 down, gesture identity not yet known, nothing sent
		PANNING,     // finger1 dragged past the dead zone -- direct camera pan, no mouse involved
		TWOFINGER,   // two fingers down -- direct camera pan (centroid) + zoom (spread), once per frame
		MOMENTUM,    // finger lifted after a fast pan -- coasting with decaying velocity, no finger involved
		PLACING      // finger1 dragged past the dead zone while a building placement is pending --
		             // anchor already sent, drag now sets rotation angle (see PlaceEventTranslator.cpp)
	};

	Phase phase = IDLE;
	SDL_FingerID finger1 = 0;
	SDL_FingerID finger2 = 0;
	float downX = 0.0f, downY = 0.0f;   // finger1 down position (window points), fixed until release
	float lastX = 0.0f, lastY = 0.0f;   // finger1 latest position (pixels)
	Uint64 downTicks = 0;

	// GeneralsX @feature Android port 01/08/2026 Native touch camera control:
	// pan/zoom go straight to TheTacticalView (userScrollBy/userZoom), driven
	// by real per-frame finger deltas -- no synthetic mouse motion, no
	// wheel-tick/RMB-drag translation. See applyCameraPan()/applyCameraZoom().
	float panLastPxX = 0.0f, panLastPxY = 0.0f; // last processed finger1 pixel pos, single-finger PANNING

	// GeneralsX @feature Android port 02/08/2026 Momentum (inertia): the
	// pixel delta actually applied on the LAST processed PANNING frame,
	// carried over as the coasting phase's starting velocity on release --
	// see applyPendingCameraMotion()'s MOMENTUM branch.
	float panVelX = 0.0f, panVelY = 0.0f;
	// A virtual "finger" position that MOMENTUM advances by panVelX/Y each
	// frame (screenToTerrain needs real screen coordinates to project, even
	// though no finger is actually there anymore).
	float momentumX = 0.0f, momentumY = 0.0f;

	// TWOFINGER tracking: both fingers' current pixel positions (updated on
	// every motion event), plus the last-processed centroid/spread so
	// applyPendingCameraMotion() can diff against them once per frame -- no
	// classification, both signals are always live.
	float f1px = 0.0f, f1py = 0.0f, f2px = 0.0f, f2py = 0.0f;
	float twoCentroidLastX = 0.0f, twoCentroidLastY = 0.0f;
	float twoDistLastPx = 0.0f;

	// TWOFINGER tap-to-cancel: frozen landing position of each finger (unlike
	// f1px/f2px above, never overwritten by later motion), so release can
	// tell "barely moved, that was a tap" from "that was a real pan/zoom".
	float twoDownX1 = 0.0f, twoDownY1 = 0.0f, twoDownX2 = 0.0f, twoDownY2 = 0.0f;

	// Double-tap tracking (single-finger taps only).
	bool   hasLastTap = false;
	Uint64 lastTapTicks = 0;
	float  lastTapX = 0.0f, lastTapY = 0.0f;
};

TouchState s_touch;

const Uint64 LONG_PRESS_MS = 600;

// GeneralsX @feature Android port 01/08/2026 Single dead-zone distance is the
// ONLY thing deciding tap vs. drag -- no settle timer (see the file-header
// @bugfix comment for why a timer was tried and removed). 16px doubles as
// both the standard Android touch-slop range and enough slack to absorb
// stationary-hold tremor while waiting out LONG_PRESS_MS for a long-press.
const float TAP_DEAD_ZONE_PX = 16.0f;

// Double-tap: select all of the clicked unit's type on screen, matching the
// PC's double-click. 350ms/40px roughly matches Android's own
// ViewConfiguration.getDoubleTapTimeout() plus slack for finger imprecision
// (two separate taps land less precisely than one continuous drag).
const Uint64 DOUBLE_TAP_MS = 350;
const float DOUBLE_TAP_DIST_PX = 40.0f;

// Two-finger tap-to-cancel: both fingers must stay within this distance of
// where they landed for the whole gesture to count as a tap (-> right-click)
// instead of a pan/zoom. A bit more generous than TAP_DEAD_ZONE_PX since two
// simultaneous fingers naturally drift a little more than one.
const float TWO_FINGER_TAP_MAX_PX = 24.0f;

// GeneralsX @feature Android port 02/08/2026 Momentum (inertia): releasing a
// single-finger pan while it's still moving fast coasts for a bit,
// decelerating, instead of the camera stopping dead the instant the finger
// lifts -- matches how map/browser-style touch scrolling normally feels.
// FRICTION is a per-frame multiplier (not per-second -- applyPendingCameraMotion
// runs once per rendered frame, so this is frame-rate-dependent same as the
// rest of this file's per-frame camera application).
const float MOMENTUM_MIN_START_PX_PER_FRAME = 2.0f;  // below this release speed, don't bother coasting
const float MOMENTUM_STOP_PX_PER_FRAME = 0.5f;       // below this, coasting has died down enough to stop
const float MOMENTUM_FRICTION = 0.92f;               // velocity *= this, every frame, while coasting

const float ZOOM_PX_PER_TICK = 40.0f; // calibration only -- see ZOOM_HEIGHT_PER_PIXEL below

// GeneralsX @feature Android port 01/08/2026 Native camera control constants.
// Pan and zoom call TheTacticalView directly (userScrollBy/userSetPosition,
// userZoom) every touch-motion event. No fake cursor, no click/drag timing
// heuristics sized for a physical mouse -- the finger IS the camera control.
//
// GeneralsX @bugfix Android port 01/08/2026 The first cut passed the
// finger's raw NORMALIZED (0..1 window-fraction) per-frame delta straight
// into userScrollBy(); the second cut fixed that to a real PIXEL delta
// divided by W3DView::scrollBy()'s own SCROLL_RESOLUTION=250 constant,
// reasoning that matching scrollBy()'s own internal divisor was sufficient.
// It wasn't: real device logs (GX_TRACE'd applyCameraPan calls during an
// actual on-device drag) showed userScrollBy() being called every motion
// event, never locked, with a nonzero delta each time -- and the camera's
// getPosition2D() moving by roughly 0.02-0.1 world units per call against a
// position around 1300-2000. A drag summing to 500+ screen pixels only
// moved the camera a few TENTHS of a world unit total: real motion, just
// three-plus orders of magnitude too small to ever be visible. scrollBy()'s
// SCROLL_RESOLUTION constant was tuned against the OLD PC RMB-drag path
// (LookAtXlat.cpp's SCROLL_RMB, its own separate SCROLL_AMT/SCROLL_MULTIPLIER
// constants, called every FRAME while held, not once per motion event) --
// matching just the one constant it shares was nowhere near enough to
// reproduce that path's actual feel, and nobody had verified the real
// on-screen result until now.
//
// Fixed by abandoning scrollBy()'s pixel-based formula entirely in favor of
// the same ground projection the engine already uses elsewhere for exactly
// this purpose (SelectionXlat.cpp's mouseover-terrain hint):
// View::screenToTerrain(). Project the finger's previous and current screen
// position to world/terrain coordinates using the CURRENT (not-yet-moved)
// camera, and move the camera by the difference -- this is dimensionally
// exact by construction (it's asking "where does this screen point actually
// sit on the ground", not guessing a pixel-to-world ratio) and adapts
// automatically to zoom level, camera angle, and screen resolution, unlike
// a fixed constant. Applied via userSetPosition() (a direct position
// setter) rather than userScrollBy(), since scrollBy()'s own pixel-delta
// reinterpretation is exactly what we're bypassing.
//
// ZOOM_HEIGHT_PER_PIXEL: calibrated against the exact sensitivity the old
// discrete wheel-tick zoom already used and was tuned for (ZOOM_PX_PER_TICK
// pixels of pinch movement == one wheel tick == View::ZoomHeightPerSecond
// world-height-units), just made continuous instead of stepped. Zoom's
// calibration was never in question -- it doesn't route through scrollBy()
// at all, and real device logs confirm userZoom() moves the camera by a
// visible amount (single-digit to low-double-digit world-height-units) on
// every call.
const float ZOOM_HEIGHT_PER_PIXEL = (float)View::ZoomHeightPerSecond / ZOOM_PX_PER_TICK;

// Applies a camera pan by projecting the finger's previous and current
// screen position onto the ground (View::screenToTerrain) and moving the
// camera by the resulting world-space difference -- see the @bugfix comment
// above for why this replaced a pixel-delta formula. Sign: the ground point
// that was under the finger before should be under the finger after (drag-
// the-map feel), so the camera moves by (worldAtOldScreenPos -
// worldAtNewScreenPos), using the CURRENT camera for both projections.
void applyCameraPan(float fromPxX, float fromPxY, float toPxX, float toPxY)
{
	if (!TheTacticalView) {
		GX_TRACE("applyCameraPan: TheTacticalView is null, dropping from(%.2f,%.2f) to(%.2f,%.2f)\n",
		         fromPxX, fromPxY, toPxX, toPxY);
		return;
	}
	if (TheShell && TheShell->isShellActive()) {
		// GeneralsX @bugfix Android port 02/08/2026 The shell (main menu,
		// including the front-end's decorative background map/cutscene) has
		// its own real TacticalView/camera sitting behind the menu widgets --
		// dragging/pinching on a menu screen has no business moving THAT
		// camera, but nothing here was checking game state at all, so it
		// did. Same check WindowXlat.cpp already uses to know the shell owns
		// input right now.
		//
		// GeneralsX @feature Android port 02/08/2026 Traced (was a silent
		// return before): device logs showed pan attempts going completely
		// unanswered -- no trace at all -- for a long stretch right at the
		// start of a session, then working perfectly for the rest of a very
		// long log with zero further gaps. That pattern (blocked once, early,
		// never again) points at TheShell still being marked active during
		// loading/match-start, not anything about being "near the command
		// center" -- this line turns that inference into a direct fact.
		GX_TRACE("applyCameraPan: blocked, TheShell->isShellActive()==true\n");
		return;
	}
	ICoord2D fromScreen, toScreen;
	fromScreen.x = (Int)fromPxX;
	fromScreen.y = (Int)fromPxY;
	toScreen.x = (Int)toPxX;
	toScreen.y = (Int)toPxY;

	Coord3D worldFrom, worldTo;
	const Bool fromOk = TheTacticalView->screenToTerrain(&fromScreen, &worldFrom);
	const Bool toOk = TheTacticalView->screenToTerrain(&toScreen, &worldTo);
	if (!fromOk || !toOk) {
		// Finger is pointing off the playable terrain (past the map edge,
		// or above the horizon at a steep camera angle) -- skip this one
		// increment rather than pan by a bogus/undefined amount.
		GX_TRACE("applyCameraPan: screenToTerrain failed fromOk=%d toOk=%d screen(%d,%d)->(%d,%d)\n",
		         (int)fromOk, (int)toOk, fromScreen.x, fromScreen.y, toScreen.x, toScreen.y);
		return;
	}

	// GeneralsX @bugfix Android port 02/08/2026 CONFIRMED root cause of the
	// "freezes unpredictably anywhere on the map, resumes later" reports
	// (device logs plus a 4-way parallel code audit, not a guess): View::
	// setPosition() -- what userSetPosition() calls -- is a bare `m_pos =
	// pos`. It never sets W3DView::m_recalcCamera, unlike the PC mouse-
	// drag path's scrollBy() (W3DView.cpp), which explicitly does. W3DView
	// ::update() only rebuilds the actual 3D camera transform (and, inside
	// that, only there invalidates screenToTerrain()'s per-pixel location
	// cache) when m_recalcCamera is true -- otherwise it can stay false for
	// an arbitrary number of frames (it only flips true incidentally, e.g.
	// when zoom or ground-height settling crosses their own thresholds,
	// which has nothing to do with where the camera is). While it's false,
	// our screenToTerrain() calls keep projecting against the SAME stale
	// transform/cache even though m_pos has already moved -- so
	// worldFrom-worldTo collapses toward zero and the pan visibly stops,
	// until something unrelated finally flips the flag and it lurches back
	// to life. Exactly matches "unpredictable in time, not tied to
	// location". Fixed by forcing the recalculation ourselves every time we
	// actually move the camera, via the same public forceRedraw() the
	// engine already exposes for this.
	//
	// GeneralsX @bugfix Android port 02/08/2026, reverted 02/08/2026 The same
	// audit also found that Generals' mission/skirmish scripting can call
	// View::setCameraLock() directly (ScriptActions.cpp, e.g. a "follow
	// this unit" cinematic trigger) to re-aim the camera at a locked object
	// every frame, and that doUserAction() never clears m_cameraLock, only
	// m_scriptedState. An earlier revision of this fix defensively cleared
	// any active camera lock here so touch dragging couldn't get stuck
	// fighting one -- but the PC mouse-drag path (userScrollBy(), same
	// doUserAction gating) has exactly the same non-clearing behavior, so a
	// real camera-lock cutscene ALREADY can't be broken out of with the
	// mouse either -- that's presumably intentional (a cutscene is supposed
	// to hold the camera). Forcibly releasing it only from touch would have
	// made touch behave inconsistently with mouse and defeated real
	// cutscene camera locks whenever one actually fires, not just the
	// hypothetical stray one this was guarding against -- never actually
	// confirmed to be the cause here. Reverted; forceRedraw() below is the
	// confirmed, sufficient fix.

	Coord3D pos = TheTacticalView->getPosition();
	pos.x += (worldFrom.x - worldTo.x);
	pos.y += (worldFrom.y - worldTo.y);
	TheTacticalView->userSetPosition(pos);
	TheTacticalView->forceRedraw();
}

// Applies a one-frame camera zoom from a change in inter-finger pixel
// distance. Fingers moving apart (distance growing) zooms in.
void applyCameraZoom(float distDeltaPx)
{
	if (!TheTacticalView || distDeltaPx == 0.0f) {
		return;
	}
	if (TheShell && TheShell->isShellActive()) {
		// See the matching check in applyCameraPan() above.
		GX_TRACE("applyCameraZoom: blocked, TheShell->isShellActive()==true\n");
		return;
	}
	const Real zoomDelta = -distDeltaPx * ZOOM_HEIGHT_PER_PIXEL;
	TheTacticalView->userZoom(zoomDelta);
	GX_TRACE("applyCameraZoom: distDeltaPx=%.2f zoomDelta=%.4f locked=%d\n",
	         distDeltaPx, zoomDelta, (int)TheTacticalView->isUserControlLocked());
}

// GeneralsX @feature Android port 01/08/2026 These three functions are the
// ENTIRE touch->engine bridge for anything that isn't direct camera control:
// each pushes exactly the GameMessage a real mouse would have produced for
// the same physical action, straight onto TheMessageStream, and nothing
// else. No SDL_Event mouse structs, no SDL3Mouse involvement, no persisted
// position -- see the file-header comment above for why this is what "no
// mouse emulation" means concretely in this engine.

ICoord2D touchPixel(float x, float y)
{
	ICoord2D p;
	p.x = (Int)x;
	p.y = (Int)y;
	return p;
}

// Hover/position hint -- WindowXlat.cpp uses this to set GUI hilite state,
// SelectionXlat.cpp uses it to build the selection-box drag region, and
// LookAtXlat.cpp uses it to know where a drag/edge-scroll anchor is. A real
// mouse's equivalent (Mouse::createStreamMessages()) sends this every
// frame from a persisted position; here it's sent only exactly when a real
// finger event gives us a real position to report.
void pushMousePosition(float x, float y)
{
	if (!TheMessageStream) {
		return;
	}
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_RAW_MOUSE_POSITION);
	msg->appendPixelArgument(touchPixel(x, y));
	msg->appendIntegerArgument(TheKeyboard ? TheKeyboard->getModifierFlags() : 0);
}

// Button down/up/double-click -- MetaEventTranslator (MetaEvent.cpp) turns a
// DOWN+UP pair into the semantic MSG_MOUSE_LEFT_CLICK/RIGHT_CLICK
// SelectionXlat.cpp actually acts on; MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK
// instead marks the NEXT up as a double-click (matching real click
// semantics exactly -- see the double-tap handling below for why DOUBLE_
// CLICK is sent instead of, not in addition to, a DOWN).
void pushMouseButton(GameMessage::Type type, float x, float y)
{
	if (!TheMessageStream) {
		return;
	}
	GameMessage *msg = TheMessageStream->appendMessage(type);
	msg->appendPixelArgument(touchPixel(x, y));
	msg->appendIntegerArgument(TheKeyboard ? TheKeyboard->getModifierFlags() : 0);
	msg->appendIntegerArgument((Int)SDL_GetTicks()); // unread by every consumer, kept for schema parity
}

void handleTouchEvent(SDL_Window *window, const SDL_Event &event)
{
	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	const float px = event.tfinger.x * (float)winW;
	const float py = event.tfinger.y * (float)winH;

	// GeneralsX @feature Android port 02/08/2026 Unconditional per-event trace
	// -- reported "panning freezes mid-drag near my command center/units,
	// have to lift and re-place my finger". Static review of applyCameraPan()
	// found nothing that treats a drawable (building/unit) differently from
	// bare terrain, and the previous log showed no run of degenerate
	// (near-zero) deltas that would explain a freeze -- so either events stop
	// arriving from SDL entirely during the freeze (an OS/driver-level
	// thing), or something resets s_touch.phase away from PANNING that this
	// file's existing traces don't cover. Logging every raw event (not just
	// the ones that end up calling applyCameraPan) answers which.
	GX_TRACE("handleTouchEvent: type=%u finger=%llu phase=%d px(%.2f,%.2f)\n",
	         (unsigned)event.type, (unsigned long long)event.tfinger.fingerID, (int)s_touch.phase, px, py);

	switch (event.type) {
	case SDL_EVENT_FINGER_DOWN:
		if (s_touch.phase == TouchState::IDLE || s_touch.phase == TouchState::MOMENTUM) {
			// A finger touching down during MOMENTUM grabs the map and stops
			// the coast immediately -- same as tapping a map mid-fling on any
			// touch device.
			//
			// Defer BUTTON output: a finger landing could become a tap, a pan,
			// a long-press, or the first finger of a two-finger gesture. A
			// premature LMB down+up is a real click to the game (e.g. it sets
			// a rally point when a production building is selected).
			s_touch.finger1 = event.tfinger.fingerID;
			s_touch.phase = TouchState::PENDING;
			s_touch.downX = s_touch.lastX = px;
			s_touch.downY = s_touch.lastY = py;
			s_touch.downTicks = SDL_GetTicks();
			// Move the cursor to the touch point NOW (motion clicks nothing, so the
			// deferred-tap protection is intact). This lets the GUI process hover
			// over the next frame(s) before the tap commits — hover-driven widgets
			// (e.g. the Generals Challenge general buttons, which are checkboxes
			// that ignore a click unless WIN_STATE_HILITED was set by a prior
			// mouse-enter) then accept the click. Real mice hover before clicking;
			// without this, a synthetic tap teleports + clicks in one instant and
			// the widget is never hilited, so only the default/first item responds.
			pushMousePosition(px, py);
		}
		else if (s_touch.phase == TouchState::PENDING || s_touch.phase == TouchState::PANNING) {
			// Second finger: always becomes direct two-finger pan+zoom
			// immediately, no classification -- see the file-header comment
			// for why. A finger landing mid-PANNING (drag-then-pinch without
			// lifting first) picks it up the same way.
			s_touch.finger2 = event.tfinger.fingerID;
			s_touch.f1px = s_touch.lastX;  // finger1's current pixel pos
			s_touch.f1py = s_touch.lastY;
			s_touch.f2px = px;             // finger2's landing pixel pos
			s_touch.f2py = py;
			s_touch.twoDownX1 = s_touch.f1px;
			s_touch.twoDownY1 = s_touch.f1py;
			s_touch.twoDownX2 = s_touch.f2px;
			s_touch.twoDownY2 = s_touch.f2py;
			s_touch.twoCentroidLastX = (s_touch.f1px + s_touch.f2px) * 0.5f;
			s_touch.twoCentroidLastY = (s_touch.f1py + s_touch.f2py) * 0.5f;
			{
				const float ddx = s_touch.f2px - s_touch.f1px, ddy = s_touch.f2py - s_touch.f1py;
				s_touch.twoDistLastPx = SDL_sqrtf(ddx * ddx + ddy * ddy);
			}
			s_touch.phase = TouchState::TWOFINGER;
		}
		// TWOFINGER with a third finger: ignored
		break;

	case SDL_EVENT_FINGER_MOTION:
		if (event.tfinger.fingerID == s_touch.finger1) {
			s_touch.lastX = px;
			s_touch.lastY = py;
			if (s_touch.phase == TouchState::TWOFINGER) {
				s_touch.f1px = px;
				s_touch.f1py = py;
			}
		} else if (s_touch.phase == TouchState::TWOFINGER && event.tfinger.fingerID == s_touch.finger2) {
			s_touch.f2px = px;
			s_touch.f2py = py;
		} else {
			break;
		}

		if (s_touch.phase == TouchState::PENDING && event.tfinger.fingerID == s_touch.finger1) {
			const float moved = SDL_fabsf(px - s_touch.downX) + SDL_fabsf(py - s_touch.downY);
			if (moved >= TAP_DEAD_ZONE_PX) {
				if (TheInGameUI && TheInGameUI->getPendingPlaceType()) {
					// GeneralsX @feature Android port 02/08/2026 Building
					// placement: a drag past the dead zone while a build is
					// pending is the rotate gesture, not a camera pan. Send the
					// button-down NOW, at the ORIGINAL press point (downX/downY)
					// -- exactly like a real mouse press, which fires before any
					// drag -- so PlaceEventTranslator anchors the building where
					// the finger first touched, not where it dragged to.
					// PlaceEventTranslator.cpp's MSG_RAW_MOUSE_LEFT_BUTTON_DOWN
					// case calls TheInGameUI->setPlacementStart() and consumes
					// the message (DESTROY_MESSAGE), so this can't also register
					// as an ordinary click/select.
					GX_TRACE("handleTouchEvent: PENDING->PLACING moved=%.2f anchor(%.2f,%.2f)\n",
					         moved, s_touch.downX, s_touch.downY);
					pushMousePosition(s_touch.downX, s_touch.downY);
					pushMouseButton(GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN, s_touch.downX, s_touch.downY);
					// Feed the current position immediately too, so the
					// rotation angle starts responding without waiting for the
					// next motion event.
					pushMousePosition(px, py);
					s_touch.phase = TouchState::PLACING;
				} else {
					// Movement always wins, however long the finger sat still
					// first -- see the file-header @bugfix comment for why this
					// no longer depends on whether LONG_PRESS_MS has elapsed.
					// Straight to a direct camera pan (TheTacticalView->
					// userScrollBy, see applyCameraPan) -- no message stream
					// involvement at all.
					GX_TRACE("handleTouchEvent: PENDING->PANNING moved=%.2f at (%.2f,%.2f)\n", moved, px, py);
					s_touch.phase = TouchState::PANNING;
					s_touch.panLastPxX = px;
					s_touch.panLastPxY = py;
				}
			}
		}
		else if (s_touch.phase == TouchState::PLACING && event.tfinger.fingerID == s_touch.finger1) {
			// Every motion event feeds PlaceEventTranslator's
			// MSG_RAW_MOUSE_POSITION case (setPlacementEnd -> rotation angle)
			// directly -- this is message-stream traffic, not a direct camera
			// call, so there's no per-frame screenToTerrain staleness concern
			// (see applyPendingCameraMotion()'s header comment, which is about
			// a completely different code path).
			pushMousePosition(px, py);
		}
		// PANNING and TWOFINGER don't apply the camera effect here -- see
		// applyPendingCameraMotion() below for why (screenToTerrain-based
		// staleness when several motion events land in the same render
		// frame).
		break;

	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		if (event.tfinger.fingerID != s_touch.finger1 &&
		    !(s_touch.phase == TouchState::TWOFINGER && event.tfinger.fingerID == s_touch.finger2)) {
			break;
		}
		{
			// Set when a TWOFINGER lift should continue as a single-finger
			// pan with the remaining finger, instead of resetting to IDLE --
			// matches any standard two-finger touch UI (lifting one finger
			// mid-pinch keeps panning with the other).
			bool continueAsSinglePan = false;
			// Set when a fast PANNING release should coast into MOMENTUM
			// instead of stopping dead -- unlike continueAsSinglePan, this
			// still needs the edge-scroll guard below (no real finger
			// remains), just not the phase reset to IDLE.
			bool startedMomentum = false;

			switch (s_touch.phase) {
				case TouchState::PENDING:
					// A CANCELED touch (incoming call, notification shade, palm
					// rejection) must not become a committed tap — that would be a
					// phantom select/command/rally-point click at the cancel point.
					if (event.type == SDL_EVENT_FINGER_CANCELED) {
						break;
					}
					if ((SDL_GetTicks() - s_touch.downTicks) >= LONG_PRESS_MS &&
					    !(TheInGameUI && TheInGameUI->getPendingPlaceType())) {
						// Still PENDING at release means it never crossed the pan
						// dead zone (crossing it is what moves phase to PANNING) --
						// held still for the whole long-press threshold, right-click
						// (issue command) at the press point. Resolved HERE, on
						// release, not proactively while still held -- see the
						// file-header @bugfix comment for why: firing it early and
						// letting a LATER drag "upgrade" the gesture is what broke
						// ordinary panning whenever a real drag started after a
						// brief pause.
						//
						// Suppressed while a building placement is pending: the
						// natural "press, study the spot, release" pause would
						// otherwise fire this and cancel the whole placement
						// (right-click always cancels pending placement, see
						// CommandXlat.cpp) instead of placing the building at the
						// default angle. Falling through to the plain-tap logic
						// below (unaffected by hold duration) places it instead --
						// see PlaceEventTranslator.cpp's MSG_RAW_MOUSE_LEFT_BUTTON_DOWN
						// case, which anchors and immediately accepts a same-point
						// click.
						pushMousePosition(s_touch.downX, s_touch.downY);
						pushMouseButton(GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN, s_touch.downX, s_touch.downY);
						pushMouseButton(GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP, s_touch.downX, s_touch.downY);
						break;
					}
					{
						// Double-tap: select all of the clicked unit's type on
						// screen, matching the PC's double-click. MetaEventTranslator
						// (MetaEvent.cpp) turns MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK +
						// the next BUTTON_UP into MSG_MOUSE_LEFT_DOUBLE_CLICK, which
						// SelectionXlat.cpp already fully handles -- we only need to
						// count taps correctly and send DOUBLE_CLICK instead of DOWN
						// for the second tap (matching exactly what a real double-
						// click's second press already does at this message level:
						// it never re-sends DOWN either, see MetaEvent.cpp).
						const float distFromLastTap = SDL_fabsf(s_touch.downX - s_touch.lastTapX)
						                             + SDL_fabsf(s_touch.downY - s_touch.lastTapY);
						const bool isDoubleTap = s_touch.hasLastTap
							&& (SDL_GetTicks() - s_touch.lastTapTicks) <= DOUBLE_TAP_MS
							&& distFromLastTap <= DOUBLE_TAP_DIST_PX;

						// Clean tap: deliver the full click at the exact press position.
						pushMousePosition(s_touch.downX, s_touch.downY);
						if (isDoubleTap) {
							pushMouseButton(GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK, s_touch.downX, s_touch.downY);
						} else {
							pushMouseButton(GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN, s_touch.downX, s_touch.downY);
						}
						pushMouseButton(GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP, s_touch.downX, s_touch.downY);

						// A completed double-tap starts a fresh sequence rather
						// than chaining into a false "triple click".
						s_touch.hasLastTap = !isDoubleTap;
						if (!isDoubleTap) {
							s_touch.lastTapTicks = SDL_GetTicks();
							s_touch.lastTapX = s_touch.downX;
							s_touch.lastTapY = s_touch.downY;
						}
					}
					break;
				case TouchState::PANNING:
					{
						// GeneralsX @feature Android port 02/08/2026 Momentum: a
						// fast release coasts, decelerating, instead of stopping
						// dead the instant the finger lifts -- see
						// applyPendingCameraMotion()'s MOMENTUM branch, which
						// keeps calling applyCameraPan() with decaying velocity.
						// No message stream involvement either way -- nothing to
						// release.
						const float speed = SDL_fabsf(s_touch.panVelX) + SDL_fabsf(s_touch.panVelY);
						if (speed >= MOMENTUM_MIN_START_PX_PER_FRAME) {
							s_touch.momentumX = s_touch.lastX;
							s_touch.momentumY = s_touch.lastY;
							s_touch.phase = TouchState::MOMENTUM;
							startedMomentum = true;
						}
					}
					break;
				case TouchState::TWOFINGER:
					// GeneralsX @feature Android port 01/08/2026, again Brought
					// back: if neither finger moved more than TWO_FINGER_TAP_MAX_PX
					// from where it landed, the whole gesture was a two-finger tap
					// -> right-click at the landing centroid (a fast "cancel
					// selection", asked back after an earlier pass dropped it as
					// "redundant" with the long-press). Otherwise, direct camera
					// control already applied via applyPendingCameraMotion() --
					// nothing to release -- and if the OTHER finger is still down, keep
					// controlling the camera with it.
					if (event.type != SDL_EVENT_FINGER_CANCELED) {
						const float move1 = SDL_fabsf(s_touch.f1px - s_touch.twoDownX1) + SDL_fabsf(s_touch.f1py - s_touch.twoDownY1);
						const float move2 = SDL_fabsf(s_touch.f2px - s_touch.twoDownX2) + SDL_fabsf(s_touch.f2py - s_touch.twoDownY2);
						if (move1 < TWO_FINGER_TAP_MAX_PX && move2 < TWO_FINGER_TAP_MAX_PX) {
							const float cx = (s_touch.twoDownX1 + s_touch.twoDownX2) * 0.5f;
							const float cy = (s_touch.twoDownY1 + s_touch.twoDownY2) * 0.5f;
							pushMousePosition(cx, cy);
							pushMouseButton(GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN, cx, cy);
							pushMouseButton(GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP, cx, cy);
						} else {
							if (event.tfinger.fingerID == s_touch.finger1) {
								s_touch.finger1 = s_touch.finger2;
								s_touch.lastX = s_touch.panLastPxX = s_touch.f2px;
								s_touch.lastY = s_touch.panLastPxY = s_touch.f2py;
							} else {
								s_touch.panLastPxX = s_touch.lastX;
								s_touch.panLastPxY = s_touch.lastY;
							}
							s_touch.finger2 = 0;
							s_touch.phase = TouchState::PANNING;
							continueAsSinglePan = true;
						}
					}
					break;
				case TouchState::PLACING:
					// GeneralsX @feature Android port 02/08/2026 Building
					// placement release. A normal lift sends the button-up,
					// which MetaEventTranslator (MetaEvent.cpp) turns into the
					// semantic MSG_MOUSE_LEFT_CLICK that PlaceEventTranslator's
					// click case actually commits (MSG_DOZER_CONSTRUCT) or, if
					// the spot turned out illegal, resets the anchor to try
					// again WITHOUT leaving placement mode -- exactly the same
					// as a real mouse press-drag-release.
					//
					// A CANCELED touch (incoming call, notification shade, palm
					// rejection) never sends a matching up, so there is no
					// later event to resolve the anchor this file already
					// committed on the dead-zone crossing -- back out of
					// placement mode entirely instead, same conservative
					// no-phantom-action rule as the PENDING case above.
					if (event.type == SDL_EVENT_FINGER_CANCELED) {
						if (TheInGameUI) {
							TheInGameUI->placeBuildAvailable(nullptr, nullptr);
						}
					} else {
						pushMouseButton(GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP, px, py);
					}
					break;
				default:
					break;
			}

			if (!continueAsSinglePan) {
				// GeneralsX @bugfix Android port 08/07/2026, narrowed 01/08/2026
				// The engine's own edge-scroll (mouse near a viewport edge
				// keeps scrolling the camera every frame) latches onto wherever the
				// last MSG_RAW_MOUSE_POSITION placed it, and a lifted finger will
				// never send another one -- so a position left near an edge would
				// scroll the camera in that direction forever. Recentering
				// UNCONDITIONALLY on every release (including an ordinary tap in
				// the middle of the screen) was fixing that but caused whatever GUI
				// widget sat at the window center to hilite after every tap,
				// anywhere on screen. Only step in when a real edge-scroll risk
				// exists; this is now the ONLY position message this file ever
				// sends without a real finger behind it.
				const float EDGE_GUARD_PX = 40.0f;
				if (s_touch.lastX < EDGE_GUARD_PX || s_touch.lastY < EDGE_GUARD_PX ||
				    s_touch.lastX >= (float)winW - EDGE_GUARD_PX || s_touch.lastY >= (float)winH - EDGE_GUARD_PX) {
					pushMousePosition((float)winW * 0.5f, (float)winH * 0.5f);
				}
				if (!startedMomentum) {
					s_touch.phase = TouchState::IDLE;
				}
			}
		}
		break;
	}
}

// GeneralsX @bugfix Android port 02/08/2026 Reported: panning freezes mid-
// drag (finger stays down and moving) specifically in visually busy areas
// (near the player's own or the enemy's buildings/units), reproducible by
// dragging out of that area (works again) and back in (freezes again) --
// present well into a match, not just at session start, and predates the
// shell-active gate. handleTouchEvent() used to call applyCameraPan()/
// applyCameraZoom() directly from inside the FINGER_MOTION case, once per
// SDL touch event -- but pollSDL3Events() drains ALL queued SDL events in
// a single `while (SDL_PollEvent())` pass each frame, and a busier scene
// (more to render -> lower FPS -> touch events queue up faster than frames
// render) means MULTIPLE motion events for the same drag land in the same
// frame. View::screenToTerrain() casts against the 3D camera's actual
// transform (W3DView::m_3DCamera, updated once per frame by
// updateCameraTransform(), gated on m_recalcCamera) -- NOT against
// View::m_pos directly. userSetPosition() updates m_pos immediately, but
// the camera transform screenToTerrain() actually rays against stays stale
// until the NEXT frame's update() runs. So the first pan call in a frame
// projects correctly, but a second, third, etc. call for more motion
// events queued in that SAME frame would ray against the now-outdated
// (pre-this-frame's-moves) transform -- computing a wrong, often near-zero
// world delta, exactly like the camera "wasn't moving" for those events.
// Fixed by moving the actual camera-effect application out of the per-
// event handler and into this function, called ONCE per frame (see its
// call site in pollSDL3Events()) using whatever position/centroid/spread
// the drained events left s_touch in -- guaranteeing at most one
// screenToTerrain "from/to" pair per frame, always against a transform
// that's consistent for both projections.
void applyPendingCameraMotion()
{
	if (s_touch.phase == TouchState::PANNING) {
		s_touch.panVelX = s_touch.lastX - s_touch.panLastPxX;
		s_touch.panVelY = s_touch.lastY - s_touch.panLastPxY;
		applyCameraPan(s_touch.panLastPxX, s_touch.panLastPxY, s_touch.lastX, s_touch.lastY);
		s_touch.panLastPxX = s_touch.lastX;
		s_touch.panLastPxY = s_touch.lastY;
	}
	else if (s_touch.phase == TouchState::TWOFINGER) {
		// Both signals, every frame, unconditionally -- see the file-header
		// @bugfix comment above (the OLDER one) for why this replaced the
		// old pan-vs-zoom classifier.
		const float cx = (s_touch.f1px + s_touch.f2px) * 0.5f;
		const float cy = (s_touch.f1py + s_touch.f2py) * 0.5f;
		applyCameraPan(s_touch.twoCentroidLastX, s_touch.twoCentroidLastY, cx, cy);
		s_touch.twoCentroidLastX = cx;
		s_touch.twoCentroidLastY = cy;

		const float dx = s_touch.f2px - s_touch.f1px, dy = s_touch.f2py - s_touch.f1py;
		const float dist = SDL_sqrtf(dx * dx + dy * dy);
		applyCameraZoom(dist - s_touch.twoDistLastPx);
		s_touch.twoDistLastPx = dist;
	}
	else if (s_touch.phase == TouchState::MOMENTUM) {
		// GeneralsX @feature Android port 02/08/2026 Coast with the velocity
		// the finger had at release, decaying every frame, using a virtual
		// "finger" (momentumX/Y) that applyCameraPan()'s screenToTerrain
		// projection still needs real screen coordinates for -- no actual
		// finger is down during this phase.
		const float newX = s_touch.momentumX + s_touch.panVelX;
		const float newY = s_touch.momentumY + s_touch.panVelY;
		applyCameraPan(s_touch.momentumX, s_touch.momentumY, newX, newY);
		s_touch.momentumX = newX;
		s_touch.momentumY = newY;
		s_touch.panVelX *= MOMENTUM_FRICTION;
		s_touch.panVelY *= MOMENTUM_FRICTION;
		const float speed = SDL_fabsf(s_touch.panVelX) + SDL_fabsf(s_touch.panVelY);
		if (speed < MOMENTUM_STOP_PX_PER_FRAME) {
			s_touch.phase = TouchState::IDLE;
		}
	}
}

} // anonymous namespace
#endif // SAGE_MOBILE_PLATFORM

namespace {

Bool DecodeNextUtf8Codepoint(const char* text, size_t length, size_t& offset, UnsignedInt& outCodepoint)
{
	outCodepoint = 0;
	if (!text || offset >= length) {
		return false;
	}

	const unsigned char first = static_cast<unsigned char>(text[offset]);
	if (first == 0) {
		return false;
	}

	if (first < 0x80) {
		outCodepoint = first;
		offset += 1;
		return true;
	}

	if ((first & 0xE0) == 0xC0 && offset + 1 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		if ((second & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x1F) << 6) | (second & 0x3F);
			offset += 2;
			return true;
		}
	}

	if ((first & 0xF0) == 0xE0 && offset + 2 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
			offset += 3;
			return true;
		}
	}

	if ((first & 0xF8) == 0xF0 && offset + 3 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80 && (fourth & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x07) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) | (fourth & 0x3F);
			offset += 4;
			return true;
		}
	}

	// Invalid UTF-8 sequence: skip one byte and keep processing.
	offset += 1;
	return false;
}

}

/**
 * Constructor: Initialize SDL3 game engine state
 */
SDL3GameEngine::SDL3GameEngine()
	: GameEngine(),
	  m_SDLWindow(nullptr),
	  m_IsInitialized(false),
	  m_IsActive(false),
	  m_IsTextInputActive(false),
	  m_TextInputFocusWindow(nullptr),
	  m_PendingTextInputRearmFrames(0)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::SDL3GameEngine() created\n");
}

/**
 * Destructor: Cleanup SDL3 resources
 */
SDL3GameEngine::~SDL3GameEngine()
{
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}

	if (m_IsInitialized) {
		// Window cleanup is done in reset/shutdown
	}
	fprintf(stderr, "DEBUG: SDL3GameEngine::~SDL3GameEngine() destroyed\n");
}

/**
 * From GameEngine: init() - initialize subsystems
 * 
 * GeneralsX @bugfix felipebraz 16/02/2026
 * Simplified to follow fighter19 pattern - SDL3/Vulkan initialized in SDL3Main.cpp
 * before GameEngine is created. This init() only delegates to parent GameEngine::init().
 * ApplicationHWnd and TheSDL3Window are already set by main() before this is called.
 */
void SDL3GameEngine::init(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::init() starting\n");

	if (TheGlobalData && TheGlobalData->m_headless) {
		// GeneralsX @bugfix Copilot 17/05/2026 Allow headless replay path to initialize engine subsystems without an SDL window.
		fprintf(stderr, "INFO: SDL3GameEngine::init() headless mode - skipping SDL window binding\n");
		m_SDLWindow = nullptr;
		m_IsInitialized = true;
		m_IsActive = true;
		GameEngine::init();
		return;
	}

	// Verify window was created by SDL3Main.cpp
	extern SDL_Window* TheSDL3Window;
	extern HWND ApplicationHWnd;
	
	if (!TheSDL3Window || !ApplicationHWnd) {
		fprintf(stderr, "FATAL: SDL3 window not initialized before GameEngine::init()\n");
		fprintf(stderr, "FATAL: TheSDL3Window=%p, ApplicationHWnd=%p\n", TheSDL3Window, ApplicationHWnd);
		return;
	}

	// Store window reference locally
	m_SDLWindow = TheSDL3Window;
	m_IsInitialized = true;
	m_IsActive = true;

#if defined(SAGE_MOBILE_PLATFORM)
	// Lifecycle events can fire outside the poll cycle on iOS/Android; catch
	// them immediately so rendering halts before the process is suspended.
	SDL_AddEventWatch(mobileLifecycleWatcher, nullptr);
#endif

	fprintf(stderr, "INFO: SDL3GameEngine using pre-initialized window\n");

	// Call parent init to initialize game subsystems
	GameEngine::init();
}

/**
 * From GameEngine: reset() - reset system to starting state
 */
void SDL3GameEngine::reset(void)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::reset()\n");
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}
	GameEngine::reset();
}

/**
 * From GameEngine: update() - per-frame update
 */
void SDL3GameEngine::update(void)
{
	pollSDL3Events();
#if defined(SAGE_MOBILE_PLATFORM)
	// Pause sim + render while backgrounded OR inactive (see mobileLifecycleWatcher).
	// Acquiring a drawable in these windows fights the OS for the surface (iOS:
	// CAMetalLayer/MoltenVK; Android: the ANativeWindow is torn down) and,
	// across repeated suspend/switcher cycles, crashes the app. Keep polling so
	// we still catch the resume events; just don't touch the GPU.
	//
	// GeneralsX @bugfix Android port 01/08/2026 Only start skipping from the
	// SECOND consecutive paused update() onward. The very first call where we
	// observe the transition still safely owns a valid ANativeWindow/swapchain
	// (pollSDL3Events() just delivered the focus-lost/background event; the OS
	// tears the surface down some time after that, not synchronously with it),
	// so let this one call finish a completely normal update+render+present.
	// Otherwise whatever GPU work was in flight the instant focus was lost is
	// what stays on screen for as long as we're backgrounded -- and that's
	// exactly what Android's task-switcher thumbnail and (on at least one
	// real device, POCO/Snapdragon 8 Elite via HyperOS) its screenshot tool
	// both read back. Real-device testing found the same "torn"-looking black
	// patch in the same spot every single time in Recents and in screenshots,
	// never during actual play -- consistent with the OS snapshotting a
	// still-mid-render frame rather than a genuine capture-time race.
	static bool s_wasPausedLastFrame = false;
	const bool pausedNow = mobileShouldPauseRendering();
	if (pausedNow && s_wasPausedLastFrame) {
		SDL_Delay(50);
		return;
	}
	s_wasPausedLastFrame = pausedNow;
#endif
	GameEngine::update();
}

/**
 * From GameEngine: execute() - main game loop
 */
void SDL3GameEngine::execute(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - entering main loop\n");
	GameEngine::execute();
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - exited main loop\n");
}

/**
 * From GameEngine: serviceWindowsOS() - native OS service
 * On Linux, process SDL3 events
 */
void SDL3GameEngine::serviceWindowsOS(void)
{
	pollSDL3Events();
}

/**
 * Check if game has OS focus
 */
Bool SDL3GameEngine::isActive(void)
{
	return m_IsActive;
}

/**
 * Set OS focus status
 */
void SDL3GameEngine::setIsActive(Bool isActive)
{
	m_IsActive = isActive;
}

/**
 * Poll and process SDL3 events
 * Handles keyboard, mouse, window, and quit events
 */
void SDL3GameEngine::pollSDL3Events(void)
{
	if (!m_SDLWindow) {
		return;
	}

#if defined(SAGE_MOBILE_PLATFORM)
	// GeneralsX @bugfix Android port 11/07/2026 - Age the tap-rearm window by one
	// game frame (not one SDL event -- pollSDL3Events() can process several events
	// per call). See m_PendingTextInputRearmFrames in SDL3GameEngine.h for why this
	// needs to be a short window rather than a same-call flag.
	if (m_PendingTextInputRearmFrames > 0) {
		--m_PendingTextInputRearmFrames;
	}
#endif

	updateTextInputState();

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
			case SDL_EVENT_QUIT:
				m_quitting = true;
				break;

			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				m_quitting = true;
				break;

			case SDL_EVENT_WINDOW_FOCUS_GAINED:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;

			case SDL_EVENT_WINDOW_FOCUS_LOST:
				m_IsActive = false;
				if (m_IsTextInputActive) {
					SDL_StopTextInput(m_SDLWindow);
					m_IsTextInputActive = false;
					m_TextInputFocusWindow = nullptr;
				}
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

#if defined(SAGE_MOBILE_PLATFORM)
			// App suspension/resume: mirror the desktop focus handling so audio
			// and mouse state pause cleanly (the render gate lives in update()).
			case SDL_EVENT_DID_ENTER_BACKGROUND:
				m_IsActive = false;
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

			case SDL_EVENT_DID_ENTER_FOREGROUND:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;
#endif

			case SDL_EVENT_WINDOW_MOUSE_ENTER:
				if (TheMouse) {
					TheMouse->onCursorMovedInside();
				}
				break;

			case SDL_EVENT_WINDOW_MOUSE_LEAVE:
				if (TheMouse) {
					TheMouse->onCursorMovedOutside();
				}
				break;

			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
				// Fighter19 pattern: direct addSDLEvent() call
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheKeyboard) {
					SDL3Keyboard* keyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
					if (keyboard) {
						keyboard->addSDLEvent(&event);
					}
				}
				break;

			case SDL_EVENT_TEXT_INPUT:
				forwardTextInputEvent(event.text.text);
				break;

			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
			case SDL_EVENT_MOUSE_WHEEL:
#if defined(SAGE_MOBILE_PLATFORM)
				// Belt-and-braces: drop SDL's own touch-synthesized mouse events.
				// handleTouchEvent() owns all touch input (pushing GameMessages
				// directly, see its file-header comment) and SDL3Mouse never
				// receives real touch events at all on mobile -- but SDL can still
				// synthesize its own mouse-shaped events from a touch if the
				// SDL_HINT_TOUCH_MOUSE_EVENTS hint didn't take for some reason;
				// drop them so they can't sneak a phantom click in some other way.
				if (event.motion.which == SDL_TOUCH_MOUSEID) {
					break;
				}
#endif
				// Fighter19 pattern: direct addSDLEvent() call with raw SDL_Event
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheMouse) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						mouse->addSDLEvent(&event);
					}
				}
				break;

#if defined(SAGE_MOBILE_PLATFORM)
			case SDL_EVENT_FINGER_DOWN:
			case SDL_EVENT_FINGER_MOTION:
			case SDL_EVENT_FINGER_UP:
			case SDL_EVENT_FINGER_CANCELED:
				// GeneralsX @bugfix Android port 11/07/2026 - A fresh touch-down or a
				// touch-up (the clean-tap case dispatches its synthetic click on UP, see
				// handleTouchEvent()) is a candidate to (re)open the on-screen keyboard.
				// Set on both since the eventual entry-field focus change is processed a
				// few frames later by GameEngine::update(), not synchronously here -- see
				// updateTextInputState() and m_PendingTextInputRearmFrames.
				if (event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_UP) {
					m_PendingTextInputRearmFrames = 20;
				}
				if (m_SDLWindow) {
					handleTouchEvent(m_SDLWindow, event);
				}
				break;
#endif

			case SDL_EVENT_WINDOW_RESIZED:
				handleWindowEvent(event.window);
				break;

			default:
				// Ignore other events for now
				break;
		}

		updateTextInputState();
	}

#if defined(SAGE_MOBILE_PLATFORM)
	// Once per frame, after every queued SDL touch event for this frame has
	// been drained -- see applyPendingCameraMotion()'s comment for why this
	// can't happen per-event.
	applyPendingCameraMotion();
#endif
}

// GeneralsX @bugfix felipebraz 01/04/2026 Enable SDL text input only while an entry gadget owns focus.
void SDL3GameEngine::updateTextInputState(void)
{
	if (!m_SDLWindow || !TheWindowManager) {
		return;
	}

	GameWindow* focusedWindow = TheWindowManager->winGetFocus();
	const Bool wantsTextInput =
		focusedWindow != nullptr && BitIsSet(focusedWindow->winGetStyle(), GWS_ENTRY_FIELD);

	if (!wantsTextInput) {
		if (m_IsTextInputActive) {
			SDL_StopTextInput(m_SDLWindow);
			m_IsTextInputActive = false;
		}
		m_TextInputFocusWindow = nullptr;
		return;
	}

	m_TextInputFocusWindow = focusedWindow;

#if defined(SAGE_MOBILE_PLATFORM)
	// GeneralsX @bugfix Android port 11/07/2026 - Only (re)open the on-screen keyboard
	// in direct response to a recent, deliberate tap (m_PendingTextInputRearmFrames),
	// never just because a field happens to be focused -- e.g. a screen's default
	// focus assignment on creation must NOT pop the keyboard on its own. An earlier
	// version of this fix polled SDL_ScreenKeyboardShown() unconditionally every
	// frame to resync after an OS-driven dismiss, but that raced the keyboard's own
	// show animation (StartTextInput() is async) and caused a stop/start fight with
	// itself several times in a row on real devices. Gating the resync to only run
	// inside this once-per-tap block fixes both: no auto-open, and no self-induced
	// flicker.
	if (m_PendingTextInputRearmFrames > 0) {
		if (m_IsTextInputActive && !SDL_ScreenKeyboardShown(m_SDLWindow)) {
			// OS dismissed it since the last tap without us being told -- resync
			// SDL's own state before asking it to show again, otherwise
			// SDL_StartTextInput() silently no-ops (it only calls into the
			// platform layer on the false->true edge of its internal flag).
			SDL_StopTextInput(m_SDLWindow);
			m_IsTextInputActive = false;
		}
		if (!m_IsTextInputActive) {
			if (SDL_StartTextInput(m_SDLWindow)) {
				m_IsTextInputActive = true;
			}
		}
		m_PendingTextInputRearmFrames = 0;
	}
#else
	if (!m_IsTextInputActive) {
		if (SDL_StartTextInput(m_SDLWindow)) {
			m_IsTextInputActive = true;
		}
	}
#endif
}

// GeneralsX @bugfix felipebraz 01/04/2026 Forward SDL UTF-8 text input through existing GWM_IME_CHAR path.
void SDL3GameEngine::forwardTextInputEvent(const char* utf8Text)
{
	if (!utf8Text || !TheWindowManager) {
		return;
	}

	// GeneralsX @bugfix felipebraz 01/04/2026 Use tracked text-input focus window to keep SDL text delivery stable.
	GameWindow* targetWindow = m_TextInputFocusWindow;
	if (!targetWindow || !BitIsSet(targetWindow->winGetStyle(), GWS_ENTRY_FIELD)) {
		return;
	}

	const size_t textLength = strlen(utf8Text);
	size_t offset = 0;
	while (offset < textLength) {
		UnsignedInt codepoint = 0;
		if (!DecodeNextUtf8Codepoint(utf8Text, textLength, offset, codepoint)) {
			continue;
		}

		// GeneralsX @bugfix felipebraz 01/04/2026 Clamp IME char forwarding to BMP and reject UTF-16 surrogate range.
		if (codepoint == 0 || codepoint > 0x10FFFFU) {
			continue;
		}

		if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
			continue;
		}

		if (codepoint > 0xFFFFU) {
			continue;
		}

		const WideChar wideCharacter = static_cast<WideChar>(codepoint);
		TheWindowManager->winSendInputMsg(targetWindow, GWM_IME_CHAR, static_cast<WindowMsgData>(wideCharacter), 0);
	}
}

/**
 * Handle keyboard event -dispatch to Keyboard manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleKeyboardEvent(const SDL_KeyboardEvent& event)
{
	// Dispatch to SDL3Keyboard if available
	if (TheKeyboard) {
		SDL3Keyboard* sdlKeyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
		if (sdlKeyboard) {
			sdlKeyboard->addSDL3KeyEvent(event);
		}
	}
}

/**
 * Handle mouse motion event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseMotionEvent(const SDL_MouseMotionEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseMotionEvent(event);
		}
	}
}

/**
 * Handle mouse button event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseButtonEvent(const SDL_MouseButtonEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseButtonEvent(event);
		}
	}
}

/**
 * Handle mouse wheel event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseWheelEvent(const SDL_MouseWheelEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseWheelEvent(event);
		}
	}
}

/**
 * Handle window event (resize, etc.)
 */
void SDL3GameEngine::handleWindowEvent(const SDL_WindowEvent& event)
{
	// TODO: Phase 2 - Handle window resize, notify graphics subsystem
	// fprintf(stderr, "DEBUG: Window event (type=%d)\n", event.type);
}

/**
 * Factory Methods for GameEngine subsystems
 * TheSuperHackers @build felipebraz 13/02/2026
 * Implementations in .cpp to provide complete type definitions and avoid circular includes
 */

LocalFileSystem *SDL3GameEngine::createLocalFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createLocalFileSystem() -> StdLocalFileSystem\n");
	return NEW StdLocalFileSystem;
}

ArchiveFileSystem *SDL3GameEngine::createArchiveFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createArchiveFileSystem() -> StdBIGFileSystem\n");
	return NEW StdBIGFileSystem;
}

GameLogic *SDL3GameEngine::createGameLogic(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameLogic() -> W3DGameLogic\n");
	return NEW W3DGameLogic;
}

GameClient *SDL3GameEngine::createGameClient(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameClient() -> W3DGameClient\n");
	return NEW W3DGameClient;
}

ModuleFactory *SDL3GameEngine::createModuleFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createModuleFactory() -> W3DModuleFactory\n");
	return NEW W3DModuleFactory;
}

ThingFactory *SDL3GameEngine::createThingFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createThingFactory() -> W3DThingFactory\n");
	return NEW W3DThingFactory;
}

FunctionLexicon *SDL3GameEngine::createFunctionLexicon(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createFunctionLexicon() -> W3DFunctionLexicon\n");
	return NEW W3DFunctionLexicon;
}

// GeneralsX @bugfix Copilot 15/04/2026 Match upstream GameEngine pure-virtual signature after sync.
Radar *SDL3GameEngine::createRadar(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy radar.
	// Upstream reference: Win32GameEngine headless factory behavior, TheSuperHackers/GeneralsGameCode
	// https://github.com/TheSuperHackers/GeneralsGameCode
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> RadarDummy (headless)\n");
		return NEW RadarDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> W3DRadar\n");
	return NEW W3DRadar;
}

// GeneralsX @bugfix Copilot 24/03/2026 Match upstream GameEngine pure-virtual signature after sync.
ParticleSystemManager* SDL3GameEngine::createParticleSystemManager(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy particle manager.
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> ParticleSystemManagerDummy (headless)\n");
		return NEW ParticleSystemManagerDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> W3DParticleSystemManager\n");
	return NEW W3DParticleSystemManager;
}

WebBrowser *SDL3GameEngine::createWebBrowser(void)
{
	// WebBrowser uses Windows COM (CComObject<W3DWebBrowser>)
	// Not available on Linux - return nullptr
	fprintf(stderr, "WARNING: WebBrowser not available on Linux platform\n");
	return nullptr;
}

/**
 * Factory method: AudioManager
 * Select audio backend based on compile flags
 * GeneralsX @bugfix Copilot 15/04/2026 Match upstream GameEngine pure-virtual signature after sync.
 */
AudioManager *SDL3GameEngine::createAudioManager(Bool dummy)
{
	(void)dummy;
	fprintf(stderr, "INFO: SDL3GameEngine::createAudioManager()\n");

#ifdef SAGE_USE_OPENAL
	fprintf(stderr, "INFO: Creating OpenAL audio backend\n");
	return new OpenALAudioManager();
#else
	fprintf(stderr, "INFO: Audio backend not available (SAGE_USE_OPENAL not defined)\n");
	fprintf(stderr, "WARNING: Falls back to parent implementation or silent mode\n");
	return GameEngine::createAudioManager();  // Call parent (may return stub)
#endif
}

#endif // !_WIN32

