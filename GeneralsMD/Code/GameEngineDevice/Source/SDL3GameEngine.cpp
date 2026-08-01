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
#include "GameClient/Mouse.h"
#include "GameClient/Keyboard.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/View.h"
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
// GeneralsX @feature Android port 01/08/2026 Native touch camera control,
// modeled on mobile-RTS convention (Rusted Warfare): camera pan and zoom are
// real touch gestures with NO mouse involvement at all -- applyCameraPan()/
// applyCameraZoom() call TheTacticalView->userScrollBy()/userZoom() directly,
// driven by actual per-frame finger deltas, not a synthesized wheel tick or
// an RMB-drag for LookAtXlat.cpp to reinterpret. Selection and commands
// still go through SDL3Mouse (mouse click/drag IS how this engine's
// selection logic and its multiplayer-sync message stream work -- see the
// design discussion this session -- there's no bypassing that queue, only
// the fake-cursor/click-timing-heuristics part is worth avoiding), but even
// there the *decision* of what gesture occurred is made once, here, from
// real touch geometry -- not guessed from mouse-shaped click/drag timing.
//
// Gestures:
//   1 finger tap                       -> select / issue command (LMB click)
//   1 finger double-tap                -> select all of that unit's type on
//                                          screen (existing double-click handling)
//   1 finger quick drag (before the long-press threshold) -> direct camera pan
//   1 finger press-and-HOLD, then drag -> selection-box drag (LMB drag)
//   1 finger long-press, released without dragging -> right-click (deselect)
//   2 fingers, one held still + the other dragging mostly vertically -> direct camera zoom
//   2 fingers moving together          -> direct camera pan (centroid delta)
//   2 fingers tapped together, neither moving -> right-click (fast RMB)
//
// GeneralsX @feature Android port 08/07/2026 Previously ANY two-finger motion
// simultaneously panned (centroid drift) AND zoomed (pinch-distance change),
// since real fingers essentially never move in perfect lockstep -- reported
// as "even a pinch both zooms and moves the camera". Two-finger gestures now
// defer (TWO_PENDING, mirroring the existing single-finger PENDING deferral)
// until per-finger displacement makes the intent unambiguous (a much larger
// commit distance + a RATIO between the two fingers' displacement, not an
// absolute cap -- see TWO_FINGER_COMMIT_PX/ZOOM_RATIO_MAX), then commit ONCE
// to pan/zoom/tap for the rest of that touch (no re-classifying mid-gesture,
// which would otherwise flicker between modes).
//
// A single finger moving past the dead zone commits to a direct camera pan
// immediately (see PANNING below) -- no artificial arming delay, since
// panning is now the obvious/safe outcome of a quick drag, not a
// selection-box. Selection-box dragging instead requires the finger to
// press-and-HOLD past the long-press threshold first, then move (see the
// LONGPRESSED branch). The synthetic mouse cursor (still used for the
// LMB/RMB-based selection and command gestures) is also recentered on every
// full release, since a lingering position near a screen edge would
// otherwise trip the engine's own edge-scroll and scroll the camera forever
// after the finger lifted.
// ---------------------------------------------------------------------------
namespace {

struct TouchState {
	enum Phase {
		IDLE,        // no fingers tracked
		PENDING,     // finger1 down, gesture identity not yet known, nothing sent
		PANNING,     // finger1 dragged before the long-press threshold -- direct camera pan, no mouse involved
		DRAGGING,    // finger1 held past the long-press threshold, then dragged -- LMB drag (selection box)
		LONGPRESSED, // long-press fired (RMB click sent); a further drag from here upgrades to DRAGGING
		TWO_PENDING, // two fingers down, pan/zoom/tap identity not yet known
		PAN,         // two-finger camera pan -- direct camera control, no mouse involved
		ZOOM         // two-finger zoom: one finger anchored, the other dragging vertically -- direct camera control
	};

	Phase phase = IDLE;
	SDL_FingerID finger1 = 0;
	SDL_FingerID finger2 = 0;
	float downX = 0.0f, downY = 0.0f;   // finger1 down position (window points)
	float lastX = 0.0f, lastY = 0.0f;   // finger1 latest position
	Uint64 downTicks = 0;
	float f1x = 0.0f, f1y = 0.0f, f2x = 0.0f, f2y = 0.0f; // normalized (0..1) per finger, latest

	// GeneralsX @feature Android port 01/08/2026 Native touch camera control:
	// pan/zoom go straight to TheTacticalView (userScrollBy/userZoom), driven
	// by real per-frame finger deltas -- no synthetic mouse motion, no
	// wheel-tick/RMB-drag translation. See applyCameraPan()/applyCameraZoom().
	float panLastNormX = 0.0f, panLastNormY = 0.0f; // last processed finger1 normalized pos, single-finger PANNING
	float panLastCentroidNormX = 0.0f, panLastCentroidNormY = 0.0f; // last processed 2-finger centroid, PAN
	float zoomLastDistPx = 0.0f; // last processed inter-finger pixel distance, ZOOM

	// Two-finger gesture classification (TWO_PENDING/PAN/ZOOM).
	float twoAnchor1X = 0.0f, twoAnchor1Y = 0.0f; // finger1 pixel pos when finger2 landed
	float twoAnchor2X = 0.0f, twoAnchor2Y = 0.0f; // finger2 pixel pos when finger2 landed

	// Double-tap tracking (single-finger taps only).
	bool   hasLastTap = false;
	Uint64 lastTapTicks = 0;
	float  lastTapX = 0.0f, lastTapY = 0.0f;
};

TouchState s_touch;

const Uint64 LONG_PRESS_MS = 600;
const float TAP_DEAD_ZONE_PX = 8.0f;   // jitter below this keeps a tap a tap

// GeneralsX @feature Android port 01/08/2026 A finger that moves past the
// dead zone BEFORE this elapses commits straight to a camera pan (see the
// PENDING branch in handleTouchEvent) -- so a quick drag always pans, no
// artificial arming delay. A finger that instead stays put THROUGH this
// window fires the long-press RMB click below (updateTouchLongPress); moving
// AFTER that point is what starts a selection-box drag instead.

// Double-tap: select all of the clicked unit's type on screen, matching the
// PC's double-click. 350ms/40px roughly matches Android's own
// ViewConfiguration.getDoubleTapTimeout() plus slack for finger imprecision
// (two separate taps land less precisely than one continuous drag).
const Uint64 DOUBLE_TAP_MS = 350;
const float DOUBLE_TAP_DIST_PX = 40.0f;

// GeneralsX @bugfix Android port 08/07/2026 Two-finger pan vs. zoom
// classification was deciding from a mere 10px of movement on whichever
// finger happened to move first, and called anything "zoom" once one
// finger's displacement was under a small ABSOLUTE threshold (18px). Real
// fingers essentially never move in perfect lockstep even during a deliberate
// two-hand pan, so ordinary pan attempts kept misfiring as zoom, while a
// genuinely "held still" finger drifts more than 18px from natural hand
// tremor while the other hand is dragging -- so genuine zoom attempts often
// fell through to pan instead. Fixed two ways: (1) wait for the more-active
// finger to move a much more meaningful distance before deciding anything at
// all (jitter noise dominates under ~10px; real intent is usually clear by
// ~30px), and (2) classify by the RATIO between the two fingers' displacement
// rather than an absolute cap on the "anchor" -- a deliberate hold-one-drag-
// the-other gesture keeps that ratio close to 0 even after 30+px of the mover
// moving, while a real two-hand pan keeps both fingers' displacement
// comparable (ratio close to 1) throughout.
const float TWO_FINGER_COMMIT_PX = 30.0f; // the MORE-active finger must move at least this far before we decide anything
const float ZOOM_RATIO_MAX = 0.28f;       // less-active/more-active displacement ratio must be below this to count as "anchored"
const float ZOOM_VERTICAL_BIAS = 1.5f;    // mover's |dy| must exceed |dx| by this factor to count as "vertical"
const float ZOOM_PX_PER_TICK = 40.0f;     // vertical pixels of mover movement per zoom step (wheel tick)

// GeneralsX @bugfix Android port 08/07/2026 The ratio check alone still let a
// genuine two-hand vertical PAN misfire as zoom whenever the two hands
// (naturally) didn't travel identical distances -- e.g. one hand moving
// 60px and the other 15px in the SAME direction satisfied ZOOM_RATIO_MAX,
// even though both were clearly cooperating on one vertical scroll rather
// than one holding still. Reject zoom whenever the "anchor" moved enough to
// rule out pure jitter AND its direction lines up with the mover's --
// incidental hand tremor has no consistent direction; a real (if smaller)
// pan contribution points the same way as the other finger.
const float CODIRECTIONAL_MIN_PX = 10.0f;      // below this, the anchor's movement is too small for direction to mean anything
const float CODIRECTIONAL_COS_THRESHOLD = 0.5f; // cosine similarity (~60 degrees) counts as "same direction" -> reject zoom, it's a pan

// GeneralsX @feature Android port 01/08/2026 Native camera control constants.
// Pan and zoom call TheTacticalView directly (userScrollBy/userZoom) every
// touch-motion event, instead of synthesizing mouse motion/wheel events for
// SDL3Mouse.cpp and the message-stream translators (LookAtXlat.cpp) to
// reinterpret. No fake cursor, no click/drag timing heuristics sized for a
// physical mouse -- the finger IS the camera control.
//
// PAN_SENSITIVITY: W3DView::scrollBy() treats its Coord2D delta as a
// screen-space fraction (internally scaled by its own SCROLL_RESOLUTION=250
// constant), so passing the finger's raw normalized (0..1 window fraction)
// per-frame delta is already the right order of magnitude; this is a small
// multiplier on top for feel, tune on-device.
const float PAN_SENSITIVITY = 1.0f;
// ZOOM_HEIGHT_PER_PIXEL: calibrated against the exact sensitivity the old
// discrete wheel-tick zoom already used and was tuned for (ZOOM_PX_PER_TICK
// pixels of pinch movement == one wheel tick == View::ZoomHeightPerSecond
// world-height-units), just made continuous instead of stepped.
const float ZOOM_HEIGHT_PER_PIXEL = (float)View::ZoomHeightPerSecond / ZOOM_PX_PER_TICK;

// Applies a one-frame camera pan from a normalized (0..1 window fraction)
// finger delta. Sign: dragging the finger left/up should make the world
// appear to follow the finger (drag-the-map feel), so the camera itself
// moves the opposite way.
void applyCameraPan(float normDX, float normDY)
{
	if (!TheTacticalView) {
		return;
	}
	Coord2D delta;
	delta.x = -normDX * PAN_SENSITIVITY;
	delta.y = -normDY * PAN_SENSITIVITY;
	if (delta.x != 0.0f || delta.y != 0.0f) {
		TheTacticalView->userScrollBy(&delta);
	}
}

// Applies a one-frame camera zoom from a change in inter-finger pixel
// distance. Fingers moving apart (distance growing) zooms in.
void applyCameraZoom(float distDeltaPx)
{
	if (!TheTacticalView || distDeltaPx == 0.0f) {
		return;
	}
	TheTacticalView->userZoom(-distDeltaPx * ZOOM_HEIGHT_PER_PIXEL);
}

void sendSyntheticMouse(SDL3Mouse *mouse, SDL_Window *window, Uint32 type,
                        float x, float y, Uint8 button = 0, float wheelY = 0.0f, Uint8 clicks = 1)
{
	// The windowID must be valid: SDL3Mouse::scaleMouseCoordinates() looks the
	// window up by id to map window points into the game's internal resolution,
	// and silently skips scaling when the lookup fails.
	const SDL_WindowID windowID = SDL_GetWindowID(window);

	SDL_Event ev;
	SDL_zero(ev);
	ev.type = type;
	switch (type) {
		case SDL_EVENT_MOUSE_MOTION:
			ev.motion.windowID = windowID;
			ev.motion.x = x;
			ev.motion.y = y;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			ev.button.windowID = windowID;
			ev.button.button = button;
			ev.button.down = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
			// clicks>=2 on a DOWN event is what SDL3Mouse.cpp promotes to
			// MBS_DoubleClick -- see the double-tap handling below.
			ev.button.clicks = clicks;
			ev.button.x = x;
			ev.button.y = y;
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			ev.wheel.windowID = windowID;
			ev.wheel.x = 0.0f;
			ev.wheel.y = wheelY;
			ev.wheel.mouse_x = x;
			ev.wheel.mouse_y = y;
			break;
	}
	mouse->addSDLEvent(&ev);
}

void handleTouchEvent(SDL3Mouse *mouse, SDL_Window *window, const SDL_Event &event)
{
	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	const float px = event.tfinger.x * (float)winW;
	const float py = event.tfinger.y * (float)winH;

	switch (event.type) {
	case SDL_EVENT_FINGER_DOWN:
		if (s_touch.phase == TouchState::IDLE) {
			// Defer all BUTTON output: a finger landing could become a tap, a
			// drag-box, a long-press, or the first finger of a two-finger
			// gesture. A premature LMB down+up is a real click to the game
			// (e.g. it sets a rally point when a production building is
			// selected).
			s_touch.finger1 = event.tfinger.fingerID;
			s_touch.phase = TouchState::PENDING;
			s_touch.downX = s_touch.lastX = px;
			s_touch.downY = s_touch.lastY = py;
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.downTicks = SDL_GetTicks();
			// Move the cursor to the touch point NOW (motion clicks nothing, so the
			// deferred-tap protection is intact). This lets the GUI process hover
			// over the next frame(s) before the tap commits — hover-driven widgets
			// (e.g. the Generals Challenge general buttons, which are checkboxes
			// that ignore a click unless WIN_STATE_HILITED was set by a prior
			// mouse-enter) then accept the click. Real mice hover before clicking;
			// without this, a synthetic tap teleports + clicks in one instant and
			// the widget is never hilited, so only the default/first item responds.
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
		}
		else if (s_touch.phase == TouchState::PENDING || s_touch.phase == TouchState::DRAGGING ||
		         s_touch.phase == TouchState::PANNING) {
			// Second finger: gesture identity (pan / zoom / two-finger tap)
			// isn't known yet -- defer, exactly like the single-finger PENDING
			// state, until movement makes the intent clear. A finger landing
			// mid-PANNING (drag-then-pinch without lifting first) re-classifies
			// the same way -- direct camera control just stops until the
			// two-finger gesture resolves to PAN or ZOOM.
			if (s_touch.phase == TouchState::DRAGGING) {
				// Finish the drag-box first.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
			}
			s_touch.finger2 = event.tfinger.fingerID;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			s_touch.twoAnchor1X = s_touch.lastX;  // finger1's current pixel pos
			s_touch.twoAnchor1Y = s_touch.lastY;
			s_touch.twoAnchor2X = px;             // finger2's landing pixel pos
			s_touch.twoAnchor2Y = py;
			s_touch.phase = TouchState::TWO_PENDING;
		}
		// LONGPRESSED / TWO_PENDING / PAN / ZOOM with extra fingers: ignored
		break;

	case SDL_EVENT_FINGER_MOTION:
		if (event.tfinger.fingerID == s_touch.finger1) {
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.lastX = px;
			s_touch.lastY = py;
		} else if ((s_touch.phase == TouchState::TWO_PENDING || s_touch.phase == TouchState::PAN ||
		            s_touch.phase == TouchState::ZOOM) && event.tfinger.fingerID == s_touch.finger2) {
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
		} else {
			break;
		}

		if (s_touch.phase == TouchState::PENDING && event.tfinger.fingerID == s_touch.finger1) {
			const float moved = SDL_fabsf(px - s_touch.downX) + SDL_fabsf(py - s_touch.downY);
			if (moved >= TAP_DEAD_ZONE_PX) {
				// GeneralsX @feature Android port 01/08/2026 Real touch camera
				// pan: moving before the long-press threshold elapses commits
				// straight to a direct camera pan (TheTacticalView->userScrollBy,
				// see applyCameraPan) -- no synthetic mouse involved at all.
				// This is the natural "quick drag moves the map" gesture.
				// Holding still past the long-press threshold instead fires the
				// existing RMB long-press (updateTouchLongPress below), and
				// dragging AFTER that point is what starts a selection-box drag
				// (see the LONGPRESSED branch below) -- the finger stayed still
				// long enough to mean "select", not "look around".
				s_touch.phase = TouchState::PANNING;
				s_touch.panLastNormX = event.tfinger.x;
				s_touch.panLastNormY = event.tfinger.y;
			}
		}
		else if (s_touch.phase == TouchState::PANNING && event.tfinger.fingerID == s_touch.finger1) {
			applyCameraPan(event.tfinger.x - s_touch.panLastNormX, event.tfinger.y - s_touch.panLastNormY);
			s_touch.panLastNormX = event.tfinger.x;
			s_touch.panLastNormY = event.tfinger.y;
		}
		else if (s_touch.phase == TouchState::LONGPRESSED && event.tfinger.fingerID == s_touch.finger1) {
			// The long-press already fired its RMB click; movement from here
			// upgrades to a selection-box drag starting at the current point,
			// instead of being swallowed. downX/downY is still the original
			// press point (untouched since FINGER_DOWN), which is exactly what
			// "did the finger actually move since it went down" needs.
			const float moved = SDL_fabsf(px - s_touch.downX) + SDL_fabsf(py - s_touch.downY);
			if (moved >= TAP_DEAD_ZONE_PX) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   px, py, SDL_BUTTON_LEFT);
				s_touch.phase = TouchState::DRAGGING;
			}
		}
		else if (s_touch.phase == TouchState::DRAGGING && event.tfinger.fingerID == s_touch.finger1) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
		}
		else if (s_touch.phase == TouchState::TWO_PENDING) {
			const float f1px = s_touch.f1x * (float)winW, f1py = s_touch.f1y * (float)winH;
			const float f2px = s_touch.f2x * (float)winW, f2py = s_touch.f2y * (float)winH;
			const float d1x = f1px - s_touch.twoAnchor1X, d1y = f1py - s_touch.twoAnchor1Y;
			const float d2x = f2px - s_touch.twoAnchor2X, d2y = f2py - s_touch.twoAnchor2Y;
			const float delta1 = SDL_sqrtf(d1x * d1x + d1y * d1y);
			const float delta2 = SDL_sqrtf(d2x * d2x + d2y * d2y);
			const float moverDeltaCandidate = (delta1 > delta2) ? delta1 : delta2;

			// Wait for the MORE-active finger to clear real movement (not just
			// jitter) before deciding anything -- see TWO_FINGER_COMMIT_PX comment.
			if (moverDeltaCandidate >= TWO_FINGER_COMMIT_PX) {
				// One-time gesture-type decision, kept for the rest of this
				// touch (no re-classifying mid-gesture).
				const bool finger1IsAnchor = delta1 <= delta2;
				const float anchorDelta = finger1IsAnchor ? delta1 : delta2;
				const float moverDelta = finger1IsAnchor ? delta2 : delta1;
				const float moverDX = finger1IsAnchor ? d2x : d1x;
				const float moverDY = finger1IsAnchor ? d2y : d1y;
				const float anchorDX = finger1IsAnchor ? d1x : d2x;
				const float anchorDY = finger1IsAnchor ? d1y : d2y;
				// Ratio, not an absolute cap: a real two-hand pan keeps both
				// fingers' displacement comparable (ratio near 1) no matter how
				// far it's gone, while a deliberate hold-one-drag-the-other
				// gesture keeps the anchor's share small (ratio near 0)
				// regardless of how vigorously the other hand moves.
				const float ratio = anchorDelta / moverDelta;
				const bool mostlyVertical = SDL_fabsf(moverDY) > SDL_fabsf(moverDX) * ZOOM_VERTICAL_BIAS;

				// GeneralsX @bugfix Android port 08/07/2026 The ratio alone still
				// misfired as zoom during a genuine two-hand vertical PAN: two
				// hands rarely travel the exact same distance, so the
				// less-active one could satisfy ZOOM_RATIO_MAX purely from
				// moving noticeably less, even though it was clearly moving in
				// the SAME direction as the other (i.e. both hands cooperating
				// on one vertical scroll, not one holding still). A true anchor
				// finger's small movement is incidental hand tremor with no
				// consistent direction; a real (if smaller) pan contribution
				// points the same way as the other finger. Reject zoom whenever
				// the "anchor" moved enough (past pure jitter) AND that
				// movement is directionally aligned with the mover's.
				bool anchorCoDirectional = false;
				if (anchorDelta >= CODIRECTIONAL_MIN_PX) {
					const float cosSim = (anchorDX * moverDX + anchorDY * moverDY) / (anchorDelta * moverDelta);
					anchorCoDirectional = cosSim >= CODIRECTIONAL_COS_THRESHOLD;
				}

				// GeneralsX @feature Android port 01/08/2026 Both branches below are
				// now direct camera control (applyCameraPan/applyCameraZoom) --
				// no synthetic RMB-down, no wheel ticks. The ratio/co-directional
				// classification above (deciding PAN vs ZOOM in the first place)
				// is unchanged; only what happens once classified is new.
				if (!anchorCoDirectional && ratio <= ZOOM_RATIO_MAX && mostlyVertical) {
					const float dx = f2px - f1px, dy2 = f2py - f1py;
					s_touch.zoomLastDistPx = SDL_sqrtf(dx * dx + dy2 * dy2);
					s_touch.phase = TouchState::ZOOM;
				} else {
					s_touch.panLastCentroidNormX = (s_touch.f1x + s_touch.f2x) * 0.5f;
					s_touch.panLastCentroidNormY = (s_touch.f1y + s_touch.f2y) * 0.5f;
					s_touch.phase = TouchState::PAN;
				}
			}
		}
		else if (s_touch.phase == TouchState::PAN) {
			const float cx = (s_touch.f1x + s_touch.f2x) * 0.5f;
			const float cy = (s_touch.f1y + s_touch.f2y) * 0.5f;
			applyCameraPan(cx - s_touch.panLastCentroidNormX, cy - s_touch.panLastCentroidNormY);
			s_touch.panLastCentroidNormX = cx;
			s_touch.panLastCentroidNormY = cy;
		}
		else if (s_touch.phase == TouchState::ZOOM) {
			// Real pinch distance between both fingers (not just one finger's
			// absolute position) -- fingers spreading apart zooms in, exactly
			// like the pinch looks, regardless of which finger the classifier
			// above happened to call the "mover".
			const float f1px = s_touch.f1x * (float)winW, f1py = s_touch.f1y * (float)winH;
			const float f2px = s_touch.f2x * (float)winW, f2py = s_touch.f2y * (float)winH;
			const float dx = f2px - f1px, dy2 = f2py - f1py;
			const float dist = SDL_sqrtf(dx * dx + dy2 * dy2);
			applyCameraZoom(dist - s_touch.zoomLastDistPx);
			s_touch.zoomLastDistPx = dist;
		}
		break;

	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		if (event.tfinger.fingerID != s_touch.finger1 &&
		    !((s_touch.phase == TouchState::PAN || s_touch.phase == TouchState::TWO_PENDING ||
		       s_touch.phase == TouchState::ZOOM) && event.tfinger.fingerID == s_touch.finger2)) {
			break;
		}
		switch (s_touch.phase) {
			case TouchState::PENDING:
				// A CANCELED touch (incoming call, notification shade, palm
				// rejection) must not become a committed tap — that would be a
				// phantom select/command/rally-point click at the cancel point.
				if (event.type == SDL_EVENT_FINGER_CANCELED) {
					break;
				}
				{
					// Double-tap: select all of the clicked unit's type on
					// screen, matching the PC's double-click.
					// MSG_MOUSE_LEFT_DOUBLE_CLICK is entirely handled already
					// (SelectionXlat.cpp) once SDL3Mouse.cpp sees clicks>=2 on
					// the DOWN event -- we only need to count taps correctly.
					const float distFromLastTap = SDL_fabsf(s_touch.downX - s_touch.lastTapX)
					                             + SDL_fabsf(s_touch.downY - s_touch.lastTapY);
					const bool isDoubleTap = s_touch.hasLastTap
						&& (SDL_GetTicks() - s_touch.lastTapTicks) <= DOUBLE_TAP_MS
						&& distFromLastTap <= DOUBLE_TAP_DIST_PX;
					const Uint8 clicks = isDoubleTap ? 2 : 1;

					// Clean tap: deliver the full click at the exact press position.
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
					                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT, 0.0f, clicks);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT, 0.0f, clicks);

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
				// Direct camera pan -- no synthetic mouse was ever pressed,
				// nothing to release.
				break;
			case TouchState::DRAGGING:
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, px, py, SDL_BUTTON_LEFT);
				break;
			case TouchState::TWO_PENDING:
				// Neither finger moved enough to become pan/zoom before one
				// lifted: a deliberate two-finger tap -> right-click (a
				// faster alternative to the single-finger long-press RMB).
				if (event.type != SDL_EVENT_FINGER_CANCELED) {
					const float cx = (s_touch.twoAnchor1X + s_touch.twoAnchor2X) * 0.5f;
					const float cy = (s_touch.twoAnchor1Y + s_touch.twoAnchor2Y) * 0.5f;
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, cx, cy);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, cx, cy, SDL_BUTTON_RIGHT);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, cx, cy, SDL_BUTTON_RIGHT);
				}
				break;
			case TouchState::PAN:
			case TouchState::ZOOM:
				// Direct camera control -- no synthetic mouse was ever
				// pressed, nothing to release.
				break;
			default:
				break;
		}

		// GeneralsX @bugfix Android port 08/07/2026 The engine's own edge-scroll
		// (mouse near a viewport edge keeps scrolling the camera every frame)
		// checks wherever the synthetic cursor was last placed. A real mouse
		// naturally isn't left sitting at the edge; a touch is -- lifting a
		// finger near the screen border otherwise left the camera scrolling in
		// that direction forever, since nothing ever told the game the "mouse"
		// moved away. Recenter it on every full release.
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, (float)winW * 0.5f, (float)winH * 0.5f);

		s_touch.phase = TouchState::IDLE;
		break;
	}
}

// Called once per engine frame (not just per touch event): a perfectly
// stationary finger produces no SDL events, so the long-press timer must be
// polled from the frame loop or it would never fire.
void updateTouchLongPress(SDL3Mouse *mouse, SDL_Window *window)
{
	if (s_touch.phase == TouchState::PENDING &&
	    (SDL_GetTicks() - s_touch.downTicks) >= LONG_PRESS_MS) {
		// No LMB was sent yet (deferred), so this is a pure right-click.
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
		                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
		                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
		s_touch.phase = TouchState::LONGPRESSED;
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
				// The gesture translator owns all touch->mouse conversion; double
				// delivery would produce phantom second clicks.
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
				if (TheMouse && m_SDLWindow) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						handleTouchEvent(mouse, m_SDLWindow, event);
					}
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
	// Poll the long-press timer every frame; a stationary finger emits no events.
	if (TheMouse && m_SDLWindow) {
		SDL3Mouse* touchMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (touchMouse) {
			updateTouchLongPress(touchMouse, m_SDLWindow);
		}
	}
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

