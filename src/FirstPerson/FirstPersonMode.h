#pragma once
/*
 * ==============================================================================================
 *  OpenXcom3D: a first-person 3D battlescape for OpenXcom Extended
 * ==============================================================================================
 *
 *  Our philosophy
 *
 *  - Faithful first. The 3D view should feel as if it had shipped with UFO: Enemy Unknown on
 *    the day the game came out.
 *  - Another camera on the same battle. The original rules decide everything: time units,
 *    energy, accuracy, line of fire, reaction fire, doors and morale. The 3D view only shows
 *    what the game does.
 *  - Only the original assets: the game's own maps, voxel shapes, sprites, palette and sounds.
 *  - Every addition is a setting. Jumping, night vision as gear, falling, FPS-style shooting
 *    and dual wield can all be switched off.
 *  - Light on the engine. Our code lives in this folder, the core only gets small hooks, and
 *    without the OpenXcom3D mod nothing changes.
 *  - Runs anywhere: a software renderer on the CPU, no graphics card needed.
 *  - Written for people: descriptive names, and a comment for every function and every block.
 *
 *  Date: 2026-10-19
 *
 *  We wish everyone all the best and all health!
 * ==============================================================================================
 */
/*
 * Copyright 2010-2026 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
/*
 * FirstPersonMode.h: the first-person mode, the controller and user interface of the 3D view
 * (its code is in the FirstPersonMode*.cpp files, one per topic).
 */
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <SDL.h>
#include "../Battlescape/Position.h"
#include "../Mod/RuleItem.h"
#include "FirstPersonVoxelModel.h"

namespace OpenXcom
{

class Action;
class Bar;
class BattleItem;
struct BattleAction;
class BattlescapeState;
class BattleUnit;
class FirstPersonView;
class FirstPersonScanner;
class FirstPersonSounds;
class Game;
class InteractiveSurface;
class NumberText;
class SavedBattleGame;
class Surface;
class Tile;
class Text;
class TextButton;

/**
 * Everything the OpenXcom3D first-person mode adds to the battlescape screen:
 * the full-screen 3D view, the top bar (Iso/3D, classic HUD, inventory, one button per
 * soldier), a small info panel, the crosshair with fire modes, mouse look vs. free cursor,
 * free turning, real-time movement paid for tile by tile with the original rules, firing,
 * the alien turn seen from your soldier, and the debug "doom mode".
 * BattlescapeState owns one of these only when an active mod enables `firstPerson:`.
 *
 * The class is big, so its code is spread over one file per topic, and this declaration
 * follows the same order:
 *
 *   FirstPersonMode.cpp               creating it, the canvas and the widgets, switching views,
 *                                     the frame (think), input (handle), drawing (blit)
 *   FirstPersonModeCamera.cpp         turning and following the soldier's eye
 *   FirstPersonModeMovement.cpp       walking, doors, jumping, lifts, flying, falling, spotting
 *   FirstPersonModeShooting.cpp       weapons, fire modes, aiming, firing, the crosshair
 *   FirstPersonModeWeaponInHand.cpp   the weapon drawn in the soldier's hands
 *   FirstPersonModeHud.cpp            the top bar, the squad buttons, the info panel, the
 *                                     soldier card and the banner
 *   FirstPersonModeSoldierStates.cpp  getting hurt, burning, going down, breaking down
 *
 * The 3D picture itself is drawn by FirstPersonView.
 */
class FirstPersonMode
{
public:
	// =====================================================================================
	// What the battlescape screen and the game call (through FirstPersonApi)
	// =====================================================================================

	/// Creates the first-person mode and adds its surfaces to the battlescape state.
	FirstPersonMode(Game *game, BattlescapeState *state);
	/// Cleans up (the surfaces belong to the state).
	~FirstPersonMode();
	/// Is the 3D view showing?
	bool isActive() const;
	/// Switches between the isometric map and the 3D view.
	void toggleView();
	/// Per-frame update.
	void think();
	/// Handles an event first; returns true if it was consumed.
	bool handle(Action *action);
	/// Draws the whole battlescape screen its own way: the 3D view, the game's surfaces (only its
	/// warnings and debug text while the classic HUD is hidden in 3D), and our overlays magnified
	/// from the canvas. Always returns true (the screen then blits nothing itself).
	bool blit();
	/// Should the mouse be captured for mouse look right now?
	bool wantsMouseCapture() const;
	/// Puts our surfaces right after the screen was resized, and remakes the 3D view at the new size.
	void resized(int widthChange, int heightChange);

	/// An FPS-style shot's next round, asked for by the game (see FirstPerson::freeAimTarget).
	static void freeAim(BattlescapeState *state, const BattleAction &action, Position &target);
	/// Where an FPS-style straight shot leaves from, asked by the game (see FirstPerson::freeAimOrigin).
	static void freeAimFrom(BattlescapeState *state, const BattleAction &action, Position &origin);
	/// Should this shot's bullet hits play quicker (an FPS-style shot of ours)?
	static bool fastShots(BattlescapeState *state, const BattleUnit *attacker);
	/// Do a shotgun's pellets fly beside its round (setting "Shotgun pellets in flight", the 3D view showing)?
	static bool pelletsInFlight(BattlescapeState *state);
	/// Setting "Full sprite hitboxes": are units hit by their pictures in this battle (see FirstPerson::spriteHitboxes)?
	static bool spriteHitboxes(BattlescapeState *state);
	/// Setting "Quick deaths": are deaths quick in this battle (see FirstPerson::quickDeaths)?
	static bool quickDeaths(BattlescapeState *state);
	/// Akimbo on the map (Ctrl+Shift+click on a hand's weapon): steps the soldier's two guns through firing together there.
	static bool toggleMapAkimbo(BattlescapeState *state, BattleUnit *unit);
	/// Akimbo on the map: its mini targets in both hands' boxes; undone once the guns no longer pair up.
	static void drawMapAkimbo(BattlescapeState *state, BattleUnit *unit, Surface *leftHand, Surface *rightHand);
	/// A shot fired on the map: with akimbo marked there, the other hand's gun fires along, as in 3D.
	static void mapAkimbo(BattlescapeState *state, BattleAction &action);
	/// Do a shotgun's pellets and a volley's two rounds each land on their own (setting "Each pellet on its own")?
	static bool eachPellet(BattlescapeState *state);
	/// The scale a stat bar is drawn at, on the map or in 3D (setting "Stat bars").
	static double statBarScale(BattlescapeState *state, int stat, int maximum, int barWidth);
	/// A shotgun's pellet landed (see FirstPerson::pelletLanded): heard, a few to a volley, and marked in the 3D view.
	static void pelletLanded(BattlescapeState *state, const Position &at, const Position &from, int impact, const BattleItem *weapon, const BattleItem *ammo, bool heard);
	/// A soldier panics or goes berserk, asked by the game (see FirstPerson::breakdown).
	static bool breakdown(BattlescapeState *state, BattleUnit *unit);
	/// A close-quarters check knocked a shot aside, told by the game (see FirstPerson::closeQuartersDeflected).
	static void deflected(BattlescapeState *state, const BattleUnit *shooter, const Position &target);
	/// Turns the view to where a shot knocked aside goes.
	void followDeflectedShot(const BattleUnit *shooter, const Position &target);
	/// Doom IDKFA: does this unit's shot spend no ammo? Asked by the game (see FirstPerson::endlessAmmo).
	static bool endlessAmmo(BattlescapeState *state, const BattleUnit *unit);
	/// Doom IDDQD: does this unit take no damage? Asked by the game (see FirstPerson::invincible).
	static bool invincible(BattlescapeState *state, const BattleUnit *unit);
	/// Where the next round of an FPS-style shot goes: along the crosshair, with a spread.
	void freeAimTarget(const BattleAction &action, Position &target);

	/// A Doom real-time fuse: when it started burning (0: not yet, the grenade is still in hand),
	/// and for how long (ms). (Public so a remade screen of the same battle can take the burning
	/// fuses over, see rememberChoices.)
	struct DoomFuse { Uint32 lit, length; };

private:
	// =====================================================================================
	// The game, the screen and switching views (FirstPersonMode.cpp)
	// =====================================================================================

	Game *_game;                           // the game: mod rules, options, palette
	BattlescapeState *_state;              // the battlescape screen our surfaces are added to
	SavedBattleGame *_save;                // the battle being played
	FirstPersonView *_view = nullptr;      // the full-screen 3D view
	FirstPersonSounds *_sounds = nullptr;  // the sounds we make ourselves (hitmarker, heartbeat, near-miss whizz...)
	bool _wants3D;                         // the player switched to 3D
	std::string _missionAtStart;           // the battle's mission type when this screen was made (another at the end: its next stage)
	bool _classicHud = false;              // original bottom HUD drawn over the 3D view
	bool _hudHidden = false;               // the 3D HUD hidden (the hide HUD key): a clear view
	bool _hudClean = false;                // and the crosshair with all round it too (the key's third step): a clean view
	bool _mouseLook = true;                // mouse turns the view (captured) instead of moving a cursor
	bool _dragLook = false;                // right button held over the view in cursor mode
	bool _zoomButtonHeld = false;          // right button held in mouse look, zooming (setting "Zoom")
	double _zoom = 1.0;                    // how much closer the view looks now: 1, gliding to ZoomFactor and back
	Surface *_scopeCross = nullptr;        // zoomed in: a thin blue cross through the reticle, across the view
	Text *_txtScoping = nullptr;           // zoomed in: "SCOPING 2.6x", blue, at the right
	bool _mouseWasCaptured = false;        // capture state seen last frame
	Uint32 _ignoreMotionUntil = 0;         // the cursor's jump to the window centre after grabbing isn't a look
	Uint32 _lastFrameTick = 0;             // when think() last ran (SDL ticks, ms)
	int _warningY;                         // where the game's warnings sit with the classic HUD (back there when leaving 3D)
	BattleUnit *_lastPlayerUnit = nullptr; // the last soldier the player had (the view stays with them in the alien turn)
	bool _musicTense = false;              // the combat track is playing
	bool _flashlightOn = true;             // the flashlight (setting "Flashlight") switched on with its key (the suit light is the game's)
	Uint32 _lastEnemySeen = 0;             // when an alien was last in sight (the combat music stops 8 s later)
	int _pausedCheats = 0;                 // the cheats the doom mode key switched off, to switch back on (bits: 1 doom mode, 2 IDKFA, 4 IDDQD)

	/// Makes the full-screen 3D view, right above the isometric map it replaces.
	void createView();
	/// Makes the crosshair, the fire mode and shot texts beside it, and the banner.
	void createCrosshairAndBanner();
	/// Makes the grav lift arrows and their LIFT PLATFORM label.
	void createLiftArrows();
	/// Makes the compass at the top right (setting "Compass").
	void createCompass();
	/// Makes the motion scanner's round screen at the bottom middle (shown while it's held up).
	void createScannerPanel();
	/// Makes the top bar's buttons: 3D, HUD, INV, previous / next soldier, the suit's buttons, END.
	void createTopBar();
	/// Makes the info panel, bottom left: name, weapon, status, the four stats.
	void createInfoPanel();
	/// Makes the soldier card shown over a squad button, with its live helmet-camera view.
	void createSoldierCard();
	/// A live feed's red dot (the soldier cam's, the map camera's), added hidden.
	Surface *makeLiveDot();
	/// Makes the soldier cam (a soldier's card's camera, enlarged with a right click), hidden.
	void createSoldierCam();
	/// Makes the soldier cam's picture (a second small 3D view) and frame at the canvas's size.
	void makeSoldierCamView();
	/// Makes the map camera's picture and frame at a size of the setting "Map camera" (1 small .. 3 large).
	void makeMapCamera(int size);
	/// Puts a surface in another's place in the screen's lists (drawing, and deleting at its end).
	void replaceStateSurface(Surface *oldSurface, Surface *newSurface);
	/// Draws a soldier's rank into an icon beside their name; true if there is one to show.
	bool drawRankIcon(Surface *icon, const BattleUnit *unit);
	/// The game's battlescape rank badge averaged down to the rank icon's size (made once).
	Surface *smallRankBadge(int sprite);
	/// Puts a remade surface of ours in the old one's place among the state's surfaces (or adds it the first time).
	void putSurfaceInPlace(Surface *oldSurface, Surface *newSurface);
	/// The colours of our panels and cards: the mod's, or black with a grey border.
	void panelColors(Uint8 &background, Uint8 &border) const;
	/// Says which screen corner each of our surfaces keeps to when the screen is resized.
	void setResizeAnchors();
	/// Selects the next soldier when the one the camera was with is gone; returns the selected unit.
	BattleUnit *selectNextSoldierIfGone(bool playerTurn);
	/// Keeps track of the soldier the view stays with during the alien turn.
	void updateLastPlayerUnit(bool playerTurn, BattleUnit *selected);
	/// Shows or hides the 3D view in place of the isometric map.
	void setActive(bool active);
	/// Notes which squad button and lift arrow the free cursor is over.
	void trackHover(Action *action);
	/// Handles a key: the view toggle anywhere, our own keys in 3D. True if it was ours.
	bool handleKey(Action *action, bool active);
	/// Handles the mouse in mouse look: turning, firing, cancelling, the fire mode wheel.
	bool handleMouseLook(Action *action);
	/// Handles the mouse with a free cursor: our buttons, lift arrows, enemy buttons, clicks on the view.
	bool handleFreeCursor(Action *action, bool active);
	/// Notes the player's choices (3D, classic HUD, the flashlight, sprinting and en passant, what
	/// the cheats and akimbo keys switch back on, the burning fuses) for a remade screen of the same battle.
	void rememberChoices() const;
	/// Shows or hides the original bottom HUD over the 3D view.
	void setClassicHud(bool on);
	/// Hides or shows the 3D HUD (the hide HUD key).
	void toggleHud();
	/// Switches the flashlight on or off (its key, with the setting "Flashlight" on).
	void toggleFlashlight();
	/// Is the 3D HUD hidden right now (in the 3D view, with the hide HUD key)?
	bool hudHidden() const;
	/// Is the view clean right now: the HUD hidden, and the crosshair with all round it too (the hide HUD key's third step)?
	bool hudClean() const;
	/// Switches between mouse look (captured mouse) and a free cursor.
	void setMouseLook(bool on);
	/// Can the player act right now (their turn, nothing else going on)?
	bool canAct() const;
	/// Is doom mode on, by itself or with one of its extras (IDKFA, IDDQD)?
	static bool doomModeOn();
	/// The doom mode key: all the cheats off, or back on as they were.
	void toggleCheats();
	/// The cheats that are on, as the info panel tags them ("DOOM MODE", "IDDQD IDKFA").
	std::string cheatTag() const;
	/// The modifier bits of the lean key while it's held as one (Shift, Ctrl or Alt; setting "Lean"), else none.
	SDLMod leanModifierHeld() const;
	/// Is a Ctrl or Alt held that is the game's (a shortcut), not the lean key?
	bool gameModifierHeld() const;
	/// Does the player's time unit reserve leave enough for a move, as the game's own walk checks it?
	bool reserveAllows(BattleUnit *unit, int timeUnits, int energy) const;
	/// Doom mode (debug): refills the soldier's time units and energy (and ammo, with IDKFA).
	void applyDoomMode(BattleUnit *unit);
	/// Doom IDDQD: keeps X-COM's units at full morale.
	void applyGodMode();
	/// In 3D, while a shot flies or something goes off (and a moment after), lets the battle's timer catch up on missed steps.
	void catchUp();
	Uint32 _catchUpUntil = 0; // the timer may catch up until then (see catchUp)
	/// Doom mode: sets off the grenades whose real-time fuse has burnt down.
	void updateDoomFuses(Uint32 now);
	/// Plays the tense combat track while aliens are in sight.
	void updateMusic(Uint32 now);

	// ---- our surfaces: laid out on a canvas, magnified on big screens ----

	int _uiScale = 1;                          // big screens: our overlays are laid out on a canvas of (screen / _uiScale) and drawn magnified
	Surface *_canvas = nullptr;                // our overlays are drawn here, then magnified (none at scale 1)
	SDL_Color _canvasPalette[256];             // the palette the canvas was given
	std::vector<Uint8> _magnifyRow;            // a canvas row widened for the screen
	std::unordered_set<Surface*> _ownSurfaces; // all our surfaces, for blitting without the classic HUD
	std::unordered_set<Surface*> _topBarSurfaces; // the top bar's (buttons, squad icons), left out while the 3D HUD is hidden
	enum Anchor { TOP_LEFT, TOP_CENTER, TOP_RIGHT, CENTER, BOTTOM_LEFT, RIGHT_CENTER };
	std::unordered_map<Surface*, Anchor> _anchors; // which screen corner each of ours keeps to on a resize
	std::vector<Text*> _textShadows;               // dark copies behind our texts, so they read over the world
	std::vector<NumberText*> _numberTextShadows;   // the same for the numbers

	/// How many times bigger our overlays are drawn on a screen of this size.
	static int uiScaleFor(int width, int height);
	/// The canvas's width: the screen's divided by the UI scale.
	int canvasWidth() const;
	/// The canvas's height: the screen's divided by the UI scale.
	int canvasHeight() const;
	/// Makes the canvas our overlays are drawn on before being magnified.
	void makeCanvas();
	/// Moves our overlays for a canvas that grew by (dX, dY), each keeping to its corner.
	void shiftAnchored(int widthChange, int heightChange);
	/// Lets a button see a mouse event in canvas coordinates.
	void handleOnCanvas(TextButton *button, Action *action);
	/// Copies a canvas-sized surface (the canvas, or the 3D view) onto the screen, magnified by the UI scale.
	void magnify(SDL_Surface *screen, SDL_Surface *source, bool transparent);
	/// Where the game's warnings go in 3D: just above the info panel.
	int warningY() const;
	/// Adds a small button in the style of the original UI.
	TextButton *makeButton(int x, int width, const std::string &label);
	/// How wide a button must be for this label.
	int labelWidth(const std::string &label) const;
	/// Adds a text with a dark shadow behind it, so it reads over the 3D view.
	Text *addText(int width, int height, int x, int y, const std::string &id, const std::string &category);
	/// Adds a number (like the original HUD's stats) with a dark shadow.
	NumberText *addNumber(int x, int y, const std::string &id);
	/// Copies every text's content and visibility onto its shadow.
	void syncShadows();

	// =====================================================================================
	// The camera (FirstPersonModeCamera.cpp)
	// =====================================================================================

	BattleUnit *_trackedUnit = nullptr;               // the unit the camera is with
	Position _trackedTile;                            // tile the camera is in (already the destination while a step is being walked)
	double _eyeX = 0.0, _eyeY = 0.0, _eyeZ = 0.0;     // the camera's eye, in voxels
	double _headBobPhase = 0.0, _headBobAmount = 0.0; // head bob while walking (an option): where in the stride, and how strong
	// up or down a level where the soldier stands (a lift, flying): the eye glides there
	Uint32 _rideStart = 0;
	Uint32 _rideTime = 0;                  // how long this glide takes
	double _rideFrom = 0.0, _rideTo = 0.0; // eye heights; _rideTo is the estimate until the unit is up
	int _rideLevel = 0;                    // the level the ride started from
	bool _rideVertical = false;            // a vertical move (lift, flying) seen last frame
	// a fall (not a lift, a ladder or flight): the eye drops as by gravity
	Uint32 _fallStart = 0;                 // when it began (0: not falling)
	double _fallEyeFrom = 0.0;             // the eye's height then
	Uint32 _landedAt = 0;                  // when the last fall landed (the view dips, see landingDip)
	/// How long going up or down a level takes (a lift, a ladder, flying, a fall), by the walk speed setting (see RideTiles).
	Uint32 rideTime() const;
	/// Is the game making this unit fall (a step into the air, the floor gone under it)?
	bool isFalling(BattleUnit *unit) const;
	/// How far the view dips just after a fall has landed, in voxels.
	double landingDip() const;

	/// Turns the view by a mouse movement.
	void turnByMouse(int mouseDeltaX, int mouseDeltaY);
	/// Eases the zoom in while the zoom key or the right button is held, and back out.
	void updateZoom(double elapsedSeconds);
	/// The field of view the 3D view shows now, in degrees: the setting's, narrowed by the zoom.
	double fieldOfViewNow() const;
	/// Turns the view with the scroll keys.
	void turnWithKeys(double elapsedSeconds);
	/// Turns the soldier to face where the camera looks (free, or by the rules).
	void faceLookDirection(BattleUnit *unit);
	double _snapTurnDegrees = 0.0;          // a turn key's 45 degrees still to turn (negative: to the left)
	bool _snapTurnFacing = false;           // a turn key was pressed: once the view has turned, the soldier faces where it looks
	double _snapPitchDegrees = 0.0;         // a level look (the classic HUD's map level buttons) still to tilt (positive: up)
	const BattleUnit *_lookAtOnSwitch = nullptr; // a unit the next soldier looked through turns to look at (an enemy button's right click, the aliens' turn cameras)
	bool _mapShowsAllLayers = false;        // the map's multi-level view, to tell when it's switched in 3D
	Uint32 _turnRefusedWarned = 0;          // when "not enough time units" was last said for a turn

	// ---- the compass (setting "Compass"), top right: where you look, the soldier's facing and,
	// turning by the rules, the head's free turn and when looking further will cost time units ----

	Surface *_compass = nullptr;                 // the dial
	Surface *_scannerPanel = nullptr;            // the motion scanner's round screen, held up at the bottom middle (see updateScanner)
	FirstPersonScanner *_scanner = nullptr;      // the motion scanner held up (or put away), and its art, ring and blips
	Uint8 _scannerRimColor = 0;                  // the round screen's rim
	Surface *_climbArrows = nullptr;             // beside it, the ways W may climb, the one it takes now lit (see updateClimbArrows)
	int _climbArrowsShown = -1;                  // what they show: up 1, down 2, lit (1 up, 2 down) times 4; -1 not drawn yet
	Text *_compassLetters[4] = {};               // N, E, S, W, going round as the view turns
	Text *_txtCompassTurn = nullptr;             // under it near the head's limit: "TURN 1 TU", or "NO TU"; or a shot's turn: "SHOT +2 TU"
	int _compassDrawn[5] = { -1000, -1, -1, -1, -1 }; // what the dial shows: the view's bearing (degrees), the facing, the warning, the shot's facing, the shot's turn warning

	/// Shows the compass, redrawn when what it shows changes.
	void updateCompass(BattleUnit *unit, bool playerTurn);
	/// Draws the dial for a view bearing (degrees, 0 north, clockwise), a facing (0..7), a warning (0 none, 1 near the limit, 2 no time units to turn) and the facing a shot will turn to (-1 none).
	void drawCompass(int viewBearing, int facing, int warning, int shotFacing);
	/// The time units the turn before a shot, throw or melee blow at what's under the crosshair will cost, and the facing it turns to.
	int shotTurnCost(BattleUnit *unit, int &direction, bool &affordable, bool *melee = nullptr) const;
	/// Turns the view towards the n-th visible enemy.
	void lookAtEnemy(int enemyIndex);
	/// Turns the view towards a unit's chest.
	void lookAtUnit(const BattleUnit *target);
	/// Turns the view to a unit, gliding there as a snap turn does.
	void glideToLookAt(const BattleUnit *target);
	/// The point on a unit the view looks at (its chest), in voxels; false for a unit off the map.
	bool chestPoint(const BattleUnit *target, double point[3]) const;
	/// Switches to the soldier who sees the n-th visible enemy with the most time units left, looking at it.
	void lookFromBestSpotter(int enemyIndex);
	/// Turns the view back to where the soldier faces, level (the classic HUD's centre button and key).
	void lookStraightAhead(const BattleUnit *unit);
	/// Tilts the view towards the level above (1) or below (-1) (the classic HUD's map level buttons).
	void lookLevelUpDown(int direction);
	/// The classic HUD's buttons that act on the isometric map, translated to the 3D view.
	bool classicHudButton(const InteractiveSurface *button, Action *action);
	/// Tells, in 3D, that the map's multi-level view was switched (it only changes the map).
	void watchMapLayers();
	/// Keeps the camera with the soldier: steps, moves, eye height, rides up and down.
	void updateCamera(BattleUnit *unit, bool canMove, double elapsedSeconds);
	/// Puts the camera in the middle of the unit's tile, at eye height.
	void snapToTile(BattleUnit *unit);
	/// The camera's eye height for a unit, in voxels.
	double eyeHeight(const BattleUnit *unit) const;

	// ---- leaning out (setting "Lean"): the lean keys held move the eye to the side, the view tilting with "Tilt" ----

	double _lean = 0.0;                     // how far into a lean: -1 all the way left .. 1 right, following the keys over LeanTime
	double _leanOffset = 0.0;               // the eye's offset to the side, in voxels (positive: right), stopped short of walls
	double _leanShift[2] = {};              // the same as a move on the map (x east, y south), in voxels

	/// Leans the eye out while a lean key is held, and works out where it goes.
	void updateLean(BattleUnit *unit, bool canLean, double elapsedSeconds, double &dropBy);
	/// How far the eye can go to one side before a wall or a unit in sight, up to the lean's reach.
	double leanRoom(BattleUnit *unit, double sideX, double sideY) const;
	/// Keeps the eye out of anything solid: to the nearest free voxel in the unit's footprint, ahead first.
	void clearEye(BattleUnit *unit, double eye[3], double yawRadians) const;
	/// Straightens up at once: no lean, no tilt.
	void endLean();
	/// Whether the soldier leans out far enough for a shot to start from that side.
	bool leaningOut() const;

	// =====================================================================================
	// Moving: walking, doors, jumping, lifts, flying, falling (FirstPersonModeMovement.cpp)
	// =====================================================================================

	// ---- walking ----

	double _velocityX = 0.0, _velocityY = 0.0; // walking momentum, voxels per second
	bool _stepPending = false;                 // a step was handed to the game's walking state and is being walked
	unsigned _mouseReleasesSeen = 0;           // the game's mouse releases known (see think: one since means another screen was on top)
	int _sidestepFacing = -1;                  // the facing a soldier sidestepping keeps (the game's strafe), while the step is walked; -1 none
	Position _stepFrom;                        // the tile that step started from
	int _stepAllowedCache[8];                  // per frame: -1 unknown, 0 blocked, 1 allowed, 2 blocked for an unsafe landing (see landingSafe)
	// a step off a ledge onto an unsafe landing (setting "Check the landing"): the edge warned about
	// (the tile and direction), when, the keys let go since, and the next push confirmed
	Position _unsafeFrom;
	int _unsafeDirection = -1;
	Uint32 _unsafeWarnedAt = 0;
	bool _unsafeReleased = false, _unsafeConfirmed = false;
	double _footstepDistance = 0.0;            // voxels walked since the last footstep
	int _footstepPhase = 0;                    // which foot is next

	/// Real-time movement with the movement keys, paying for each tile entered by the rules.
	void updateMovement(BattleUnit *unit, bool canMove, double elapsedSeconds);
	/// Could the unit step into the neighbouring tile in this direction now, by the rules?
	bool stepAllowed(BattleUnit *unit, int stepX, int stepY);
	/// Hands one step to the game's walking state; false if it couldn't start.
	bool commitStep(BattleUnit *unit, int stepX, int stepY);
	/// Keeps a coordinate inside a tile, away from its edges.
	double clampInsideTile(int tileCoordinate, double voxelCoordinate, int sizeOffset) const;
	/// Plays footstep sounds as the camera walks.
	void playFootsteps(BattleUnit *unit, double distance);

	// ---- doors and diagonal gaps: steps that move the camera on their own ----

	bool _guidedStep = false;        // the pending step moves the camera on its own: through a door or a diagonal gap
	bool _guidedStepDoor = false;    // it goes through a door: wait for it to open first
	int _guidedStepDirection[2] = {}; // the step's direction (x, y: -1, 0 or 1)
	Uint32 _guidedStepStart = 0;     // when the step started (the camera waiting at the door)

	/// Is there a closed door between the camera's tile and the next one this way?
	bool closedDoorTowards(int stepX, int stepY, int z) const;
	/// Pushing into a corner whose diagonal step is open but both sides blocked: takes that step.
	bool stepThroughDiagonalGap(BattleUnit *unit, double newEyeX, double newEyeY, double wantedVelocityX, double wantedVelocityY, bool sidesMayBeOpen);
	/// Walks the camera through a guided step (after a door has opened).
	bool walkGuidedStep(BattleUnit *unit, double &newEyeX, double &newEyeY, double elapsedSeconds);
	/// Opens the door the player points at (or the nearest one in view) within the soldier's reach, without walking through it.
	void openDoor(BattleUnit *unit);
	/// The facing that opens the door the player points at, or else the closed one within reach nearest the view; -1 for none.
	int doorWithinReach(BattleUnit *unit);
	/// Opens the door once the paid turn towards it (turning by the rules) is done.
	void openDoorAfterTurn(BattleUnit *unit);
	/// The game's turn on the spot that opens the door the soldier faces, at its usual cost.
	void openDoorAhead(BattleUnit *unit);
	BattleUnit *_doorAfterTurnUnit = nullptr; // turning by the rules towards a door: the soldier turning
	int _doorAfterTurn = -1;                  // and the facing to open the door from (-1: no door waits)

	// ---- jumping: a free hop of the camera; into a low obstacle, a vault over it (the step is
	// walked by the rules, the pathfinding letting that obstacle through) ----

	Uint32 _jumpStart = 0;                       // 0: on the ground
	Uint32 _jumpTime = 0;                        // how long this jump lasts, ms
	double _jumpHeight = 0.0;                    // voxels at the top
	bool _vaulting = false;                      // the camera glides over the obstacle
	double _vaultFrom[2] = {}, _vaultTo[2] = {}; // where the glide over the obstacle starts and lands (x, y in voxels)

	/// Jumps, or vaults over a low obstacle straight ahead.
	void jump(BattleUnit *unit);
	/// Vaults over a low obstacle straight ahead, if there is one.
	bool tryVault(BattleUnit *unit, int stepX, int stepY);
	/// Is this tile part low enough to jump over?
	bool isLowObstacle(const Tile *tile, int part) const;
	/// Moves the camera over the obstacle during a vault.
	bool glideOverObstacle(BattleUnit *unit, double &newEyeX, double &newEyeY);
	/// How high the jump has lifted the eye just now.
	double currentJumpHeight(BattleUnit *unit);
	/// Ends a vault: the pathfinding goes back to the rules, the camera to the keys.
	void endVault();

	// ---- lifts: a grav lift (or ladder) under the soldier shows purple arrows up and down with
	// LIFT PLATFORM between them; a click (free cursor) or the up/down keys ride it ----

	Surface *_liftUp = nullptr, *_liftDown = nullptr; // the up and down arrows
	Text *_txtLift = nullptr, *_txtLiftKeys = nullptr; // "LIFT PLATFORM", and the keys that ride it
	int _liftArrowHovered = 0;  // 1 over the up arrow, -1 over the down arrow
	int _liftKeysShownFor = -1; // which arrows the keys line was written for (bit 1 up, bit 2 down)
	Uint32 _climbRetryAt = 0;   // a climb with W the game refused (time units, say): tried again from then (SDL ticks), not every frame

	/// Shows the lift arrows while the soldier stands on a grav lift.
	void updateLift(BattleUnit *unit, Uint32 now);
	/// Which ways a grav lift or a ladder takes the soldier from here.
	void liftWays(BattleUnit *unit, bool &up, bool &down) const;
	/// Which ways W and S may climb from here: a lift's or a ladder's, or a flyer's with flight on.
	void climbWays(BattleUnit *unit, bool &up, bool &down) const;
	/// The climb arrows beside the compass: the ways W may climb, the one it takes now lit.
	void updateClimbArrows(BattleUnit *unit);
	/// W and S looking up or down on a lift or a ladder: climbs; true if that took the push.
	bool updateClimb(BattleUnit *unit, double forward);
	/// Draws a pulsing purple lift arrow, up or down.
	void drawLiftArrow(Surface *surface, bool up, bool hover, Uint32 now);
	/// Which lift arrow is at this canvas point: 1 up, -1 down, 0 none.
	int liftArrowAt(double x, double y) const;

	// ---- flying and falling ----

	std::unordered_map<const BattleUnit*, bool> _flightSwitchedOn; // flight switched on, per soldier
	bool _landing = false;                              // flight switched off in the air: coming down a level at a time
	const BattleUnit *_landingUnit = nullptr;           // the soldier landing (also while interrupted: another soldier looked through)
	int _landingResumeTurn = 0;                         // held up (blocked, out of time units): the landing goes on from this turn
	bool _landAfterStep = false;                        // flight off, stepping off a ramp's edge: land once there
	int _fallFromZ = -1;                                // a step off a ledge: the level it was taken from (fall damage on landing)

	/// Can this unit fly (or swim) at all?
	bool canFly(const BattleUnit *unit) const;
	/// Is flight switched on for this soldier?
	bool flightOn(const BattleUnit *unit) const;
	/// Switches flight on or off.
	void toggleFlight(BattleUnit *unit);
	/// May a walker go up or down a level from here (a lift ride, a ladder)?
	bool walkerMayMoveUpDown(const Position &position, int direction) const;
	/// Goes up or down a level: by grav lift or ladder, or flying (the up and down keys also switch flight on and off).
	void moveUpDown(BattleUnit *unit, int direction, Action *action, bool upDownKey = false);
	/// Flight each frame: its cost, and coming down after it's switched off in the air.
	void updateFlight(BattleUnit *unit);
	/// Hurts the soldier for a fall of some levels.
	void fallDamage(BattleUnit *unit, int levels);

	// ---- spotting a new enemy stops the walk, like it interrupts a move on the isometric map ----

	BattleUnit *_spottingUnit = nullptr; // the unit whose enemies in sight we count
	size_t _enemiesSpottedCount = 0;     // how many it had in sight last frame
	bool _movementHeld = false;          // until the movement keys are let go

	// ---- tile by tile (setting "Movement": Tiles): a key press walks one tile ----

	bool _tileMove = false;              // a move to the next tile is under way
	bool _tileMoveQuick = false;         // it's a numpad move: almost at once
	Position _tileMoveTarget;            // the tile it goes to (x, y)
	int _numpadMove[2] = {};             // a numpad key pressed for a move: forward (1, -1), right (1, -1); both 0 for none
	Uint32 _numpadPressAt = 0;           // when it was pressed (a second key within NumpadComboWait joins it: a diagonal)
	Uint32 _tileMoveKeysSince = 0;       // when the movement keys were pressed (a moment's wait catches a second key, for a diagonal)
	double _tileMoveClosest = 0.0;       // the closest the camera has come to the target's middle, voxels
	Uint32 _tileMoveProgressAt = 0;      // when it last came closer
	bool _tileMoveHeldOn = false;        // W, A, S or D held without a break since the last move started (walks on, but not through a closed door)
	bool _keysHeldThroughAction = false; // move keys held while the game was busy with something else (a shot): no move until let go

	/// The movement keys' push: forward (1) or back (-1), right (1) or left (-1).
	void movementKeys(double &forward, double &strafe) const;
	/// The numpad's push for a move (8 ahead, 2 back, 4 left, 6 right), held right now.
	void numpadKeys(int &forward, int &strafe) const;
	/// Tile moves: the velocity that walks the camera to the next tile's middle; starts and ends the moves.
	bool tileMoveVelocity(BattleUnit *unit, double forward, double strafe, bool canMove, double walkSpeed, double elapsedSeconds, double &velocityX, double &velocityY);
	Uint32 _spottedUntil = 0;             // "enemy spotted" banner

	/// Stops the walk when a new enemy comes into view (not en passant).
	void checkSpotted(BattleUnit *unit, bool playerTurn);

	// ---- two ways of walking, each switched on and off with its key: sprinting (the game's
	// run, at a faster pace) and en passant (walking on past enemies coming into sight, as Shift
	// does on the map); each shows a line under the compass while on ----

	bool _sprintOn = false;              // sprinting switched on (key "Sprint")
	bool _sprintStepRan = true;          // the last step was a run (false once a run couldn't be paid for: walking pace then)
	bool _enPassantOn = false;           // en passant switched on (key "En passant")
	Text *_txtSprint = nullptr;          // "SPRINT" under the compass while sprinting is on
	Text *_txtEnPassant = nullptr;       // "EN PASSANT" under that while en passant is on

	/// Switches sprinting on or off.
	void toggleSprint();
	/// Switches en passant on or off.
	void toggleEnPassant();
	/// Is this step the game's run (sprinting, and the soldier can run it), not a walk?
	bool runsStep(BattleUnit *unit, const Position &from, int direction) const;
	/// Makes the lines under the compass for the ways of walking, hidden.
	void createWalkModeLines();
	/// Shows the lines for the ways of walking switched on, under the compass (3D only).
	void updateWalkModeLines(BattleUnit *unit);

	// =====================================================================================
	// Weapons, fire modes, aiming and firing (FirstPersonModeShooting.cpp)
	// =====================================================================================

	std::unordered_map<const BattleUnit*, int> _chosenHand; // each soldier's hand picked with G (or on the classic HUD): 0 right, 1 left, 2 built-in weapon
	std::unordered_map<int, DoomFuse> _doomFuses;           // Doom mode: grenades armed in real time (by item id)
	/// A grenade on a timer the Doom real-time fuse can take (not a proximity grenade, not one that goes off on landing).
	static bool timedGrenade(const BattleItem *item);
	/// Pulls a grenade's pin for the Doom real-time fuse (primed by the game, the click, the fuse burning from a moment or not yet).
	void pullDoomPin(BattleUnit *unit, BattleItem *grenade, Uint32 lit);
	// Doom free grenades (setting "Doom free grenades with airburst"): one flying by the 3D view's own throw
	struct FreeGrenade { BattleItem *item; BattleUnit *thrower; float position[3], velocity[3]; Uint32 lastTick; }; // voxels; voxels a second
	FreeGrenade _freeGrenade = {};
	/// Throws a grenade freely at a point (Doom free grenades): out of the hand, flying from the next tick.
	void throwFreeGrenade(BattleUnit *unit, BattleItem *grenade, const Position &aimedVoxel);
	/// Flies the free grenade on to now; false once it has landed or burst (the flight is over).
	bool stepFreeGrenade(Uint32 now);
	BattleActionType _fireMode = BA_NONE;               // fire mode for the left mouse button
	const BattleItem *_fireModeWeapon = nullptr;        // the weapon _fireMode was chosen for
	Position _aimVoxel;                                 // what's under the crosshair
	bool _aimHitsSomething = false;                     // something is under the crosshair (not just sky)
	// the path the next throw or shot would take (setting "Show trajectory"), and a target marked
	// for confirm fire (setting "Confirm fire"): the next click on the same tile fires
	std::vector<int> _trajectoryKey;                    // what the path shown was worked out for (empty: none shown)
	Position _confirmTile = Position(-1, -1, -1);       // the marked target tile (x -1: none)
	Position _confirmVoxel;                             // the point marked on it
	Position _confirmFrom;                              // where the soldier stood when it was marked
	const BattleUnit *_confirmUnit = nullptr;           // the soldier who marked it
	const BattleItem *_confirmWeapon = nullptr;         // with this weapon...
	BattleActionType _confirmMode = BA_NONE;            // ...and this mode
	bool _burstConfirmed = false;                       // the held trigger's bursts after a confirmed one need no confirming
	/// Shows the path the next throw or shot would take (or the marked target's), worked out as the game will.
	void updateTrajectory(BattleUnit *unit, bool playerTurn);
	/// The curve the game's own aim finds for a throw or an arcing shot at a tile, if any (its path, if asked for).
	bool gameArcFound(BattleAction &shot, const Position &targetTile, std::vector<Position> *path);
	/// The curve of an arcing shot forced at a tile (as with Ctrl held), if any (its path, if asked for).
	bool forcedArcFound(BattleAction &shot, const Position &targetTile, std::vector<Position> *path);
	/// Is this an arcing shot the game would refuse, which the setting "Force arcing shots" fires forced?
	bool forcesArc(BattleAction &shot, const Position &targetTile);
	/// The tile a point under the crosshair aims at (a unit's head poking up into the tile above: the unit's own tile).
	Position aimedTile(const Position &aimedVoxel) const;
	/// Does the game let a straight shot fly along a line (it meets the target tile first, or nothing)?
	bool gameLetsLineFly(BattleAction &shot, const Position &origin, const Position &voxel, const Position &targetTile);
	/// The game's own aim at a tile for a straight shot, with off-centre tries; whether it finds a line of fire.
	bool gameAimFound(BattleAction &shot, const Position &targetTile, Position &origin, Position &voxel);
	/// How a straight shot with the game's own targeting goes (see aimFor).
	enum ShotAim { AIM_EXACT, AIM_GAME, AIM_FORCED, AIM_REFUSED };
	/// A straight shot with the game's own targeting: at the exact point, the game's aim, forced, or refused; and its line.
	ShotAim aimFor(BattleAction &shot, const Position &targetTile, const Position &aimedVoxel, Position &origin, Position &voxel);
	/// Forgets the target marked for confirm fire.
	void clearConfirmMark() { _confirmTile = Position(-1, -1, -1); _confirmUnit = nullptr; _confirmWeapon = nullptr; _confirmMode = BA_NONE; }

	/// The weapon in the chosen hand (or the other hand if that one is empty).
	BattleItem *activeWeapon(BattleUnit *unit) const;
	/// The hand the soldier fires with: 0 right, 1 left, 2 the built-in weapon, -1 none.
	int activeHand(BattleUnit *unit) const;
	/// What a hand gives: what it holds, else the empty hand's weapon; or (2) the built-in weapon.
	BattleItem *handWeapon(BattleUnit *unit, int hand) const;
	/// The unit's empty-hand weapon (fists, a claw: OXCE's specialUseEmptyHand), found as the game finds it.
	BattleItem *emptyHandWeapon(BattleUnit *unit) const;
	/// The hand a weapon is used from: 0 right, 1 left, 2 built-in.
	int handOf(BattleUnit *unit, const BattleItem *weapon) const;
	/// The hand's letter on the panel: "R: ", "L: ", or "S: " for the built-in (special) weapon.
	std::string handLetter(BattleUnit *unit, const BattleItem *item) const;
	/// Leaning: the side a shot starts from, by where the eye is in the tile (setting "Lean shots", or leaning out).
	BattleActionOrigin leanSide(const BattleUnit *unit, const Position &targetTile) const;
	/// Melee at a body lying in front or at the feet (setting "Melee hits walls and the downed").
	bool meleeDowned(BattleUnit *unit, BattleItem *weapon, BattleAction *action);
	/// Melee at the floor or ceiling under the crosshair (setting "Melee hits floors"), by the game's own hit.
	void meleeFloor(BattleUnit *unit, BattleItem *weapon, BattleAction *action, const Position &struck);
	/// A melee blow's power and damage type, as the game works them out.
	int meleeBlowPower(const BattleActionAttack &attack, const RuleDamageType *&damageType) const;
	/// A blow made by the 3D view's own pieces lands: the weapon's melee sound, and the weapon in hand swings.
	void meleeBlowLanded(BattleItem *weapon);
	/// The wall, door or object (or floor, if asked) the crosshair meets within a melee blow's reach, on the unit's own tile or a neighbouring one.
	bool crosshairTerrainMelee(BattleUnit *unit, Position &struck, int &part, bool floors = false) const;
	/// Whether a melee blow may strike terrain: the mod's terrain melee or the setting on, and the weapon damaging terrain.
	bool meleeStrikesTerrain(BattleItem *weapon) const;
	/// Whom a melee blow would strike now (see meleeTarget).
	struct MeleeAim
	{
		BattleUnit *target = nullptr; // a unit under the crosshair beside the soldier (null: none)
		Position tile;                // the tile it's struck on
		int direction = -1;           // the way the soldier turns to face it first
		std::string refusal;          // a unit under the crosshair out of reach: why (the warning shown)
	};
	/// Whom a melee blow would strike now, by the crosshair: a unit beside the soldier, or out of range.
	MeleeAim meleeTarget(BattleUnit *unit) const;
	/// Turns the soldier to face a way before using something on a neighbour (by the rules, or for free); false without the time units for the turn and the use.
	bool turnBeforeUse(BattleUnit *unit, int direction, int useTime);
	/// Akimbo: the gun in the other hand that fires along, or null.
	BattleItem *offHandWeapon(BattleUnit *unit, const BattleItem *weapon) const;
	/// Has a unit lost health in this battle (not only carried over from earlier fights)?
	bool hurtThisBattle(const BattleUnit *unit) const;
	/// Is the landing of a step off a ledge safe: ground to walk on, with a step leading out?
	bool landingSafe(BattleUnit *unit, const Position &ahead) const;
	/// A push into a step off a ledge refused for an unsafe landing: the warning, or (pushed again) the go-ahead.
	void pushedIntoUnsafeLanding(int direction, bool deliberate);
	/// The akimbo key: full or realistic akimbo off, or the same one back on.
	void toggleAkimbo();
	/// The akimbo style key: both guns at once, or one a moment after the other (setting "Akimbo in turn").
	void toggleAkimboStyle();
	/// Dual wield: the other hand's gun if it can fire along with this mode, and the mode it fires.
	BattleItem *dualPartner(BattleUnit *unit, const BattleItem *weapon, BattleActionType mode, BattleActionType &partnerMode) const;
	/// Akimbo on the map: do the soldier's two guns pair up there?
	bool mapAkimboPairs(const BattleUnit *unit) const;
	/// Akimbo on the map: the other hand's gun that fires along with this mode there, and the mode it fires.
	BattleItem *mapAkimboPartner(BattleUnit *unit, const BattleItem *weapon, BattleActionType slot, BattleActionType &partnerMode) const;
	/// Dual wield: the other hand's gun that fires along with a shot (dualPartner's, with time units for both), or null.
	BattleItem *pairedGun(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, BattleActionType &offMode) const;
	/// Akimbo: do the two guns fire in turn (either one's shot arcs) rather than together?
	bool firesInTurn(const BattleItem *weapon, BattleActionType mode, const BattleItem *otherWeapon, BattleActionType otherMode) const;
	/// Akimbo in turn: the other gun's shot, waiting for the game to be done with the first (see fireTurnShot).
	struct TurnShot
	{
		BattleUnit *unit = nullptr;     // the soldier
		BattleItem *weapon = nullptr;   // the other gun (null: no shot waiting)
		BattleActionType mode = BA_NONE; // its fire mode
		Position targetTile, aimedVoxel; // what the first shot was aimed at
		bool hitSomething = false;      // the aim met something there (not the sky)
	} _turnShot;
	const BattleItem *_turnShotFired = nullptr; // the gun that just fired in turn: the game aims with it until endShotAim, which isn't a pick (see updateFireMode)
	/// Akimbo in turn: fires the other gun's shot once the game is done with the first, or calls it off.
	void fireTurnShot();
	/// Fires a shot or a throw at a tile through the game's own action, the other gun joining in (or not).
	void launchShot(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, const Position &targetTile, const Position &aimedVoxel, bool hitSomething, BattleItem *otherWeapon, BattleActionType otherMode);
	/// The fire modes the left mouse button can use with an item, in wheel order.
	std::vector<BattleActionType> fireModesFor(BattleUnit *unit, BattleItem *weapon) const;
	/// Keeps the fire mode valid for the weapon in hand.
	void updateFireMode(BattleUnit *unit);
	/// Switches to the next or previous fire mode.
	void cycleFireMode(int step);
	/// Picks a fire mode directly, if the weapon has it.
	void selectFireMode(BattleActionType mode);
	/// Switches the weapon hand.
	void switchHand();
	/// Grabs the lone item at the soldier's feet into a free hand, or opens the inventory.
	void grabItemUnderfoot(Action *action);
	/// Notes what's under the crosshair this frame.
	void updateAim(BattleUnit *unit);
	/// A spray burst being marked: its points and the next one, for the view to draw.
	void updateSprayMarks(BattleUnit *unit);
	/// Whether the line of the action's other costs shows under the crosshair (the unit name goes a line lower).
	bool costLineShown() const;
	/// How many lines show under the shot line (the damage range, the other costs): 0 to 2.
	int linesUnderShot() const;
	/// The shot's damage range by the crosshair, as the map shows it with Alt: "30-90"; empty for none.
	std::string shotDamage(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, int &color) const;
	/// The text colour of a damage type (the ammo tag's and the damage range's).
	Uint8 damageTypeColor(ItemDamageType type) const;
	/// Whether the accuracy line shows under the crosshair (the unit name and a downed unit's icons go under it).
	bool accuracyLineShown() const;
	/// The first voxel along the ray through a pixel, as a shot would see it; true if something is there.
	bool aimAt(int pixelX, int pixelY, BattleUnit *unit, Position &voxel, int *voxelType = nullptr) const;
	/// Does a line through a point, carried on past it, meet the shot's target first (its tile, a wall on its near side, or its unit)?
	bool lineThroughMeetsTarget(BattleAction &shot, const Position &origin, const Position &point, const Position &targetTile, Position &hit);
	/// How far along the crosshair's ray a shot at the open sky aims, in voxels (see aimAt).
	double skyReach(BattleUnit *unit) const;
	/// Uses the fire mode on whatever is under a pixel of the view, through the game's own actions.
	void fire(int pixelX, int pixelY);
	/// Whom a medi-kit would treat now (see medikitPatient).
	struct MedikitAim
	{
		BattleUnit *patient = nullptr; // the patient (null: none)
		int direction = -1;            // the way the soldier turns to face them first (-1: no turn)
		std::string refusal;           // with no patient, why (the warning shown)
	};
	/// Whom a medi-kit would treat now, by the crosshair: a unit beside the soldier, a body at their feet, or themself.
	MedikitAim medikitPatient(BattleUnit *unit, BattleItem *medikit) const;
	/// Uses a medi-kit (no action menu) on the patient pointed at, or a scanner, as the game's menu would.
	void useItemDirectly(BattleUnit *unit, BattleItem *item);
	/// Tosses a tactical flare at the crosshair.
	void throwFlare();

	// ---- FPS-style combat (setting "Faithful X-COM targeting" off): rounds go along the
	// crosshair with a spread, recoil lifts the aim, holding the trigger in auto keeps firing ----

	bool _triggerHeld = false;                   // the left button is held down (auto keeps firing)
	bool _shotAimLeft = false;                   // a shot of ours was handed to the game, which keeps aiming it (see endShotAim)
	/// A shot is under way: ours (its burst, its hits) or anyone's round or blast.
	bool shotUnderWay() const;
	Uint32 _recoilShotSeen = 0, _lastRecoil = 0; // our last shot the recoil reacted to; when the view was last kicked up
	double _recoilDebt = 0.0;                    // degrees the recoil lifted the view, eased back after firing
	double _bloom = 0.0;                         // extra spread from the last rounds (degrees, 95% circle)

	/// Is this fire mode FPS style just now?
	bool freeAimOn(BattleActionType mode) const;
	/// How wide an FPS-style shot spreads, in degrees (95% of rounds).
	double spreadFor(const BattleUnit *unit, BattleActionType mode, bool dual = false) const;
	/// FPS-style recoil: lifts the view with each round, then eases it back.
	void updateRecoil(BattleUnit *unit, double elapsedSeconds);
	/// FPS-style auto: fires the next burst while the trigger is held.
	void keepFiringWhileHeld(BattleUnit *unit, bool playerTurn);
	/// Stops the game aiming our last shot again, once it's done with it.
	void endShotAim();

	// ---- the crosshair, with the fire mode and the shot's odds and cost under it ----

	Surface *_crosshair = nullptr;                 // the crosshair's surface
	Uint64 _crosshairDrawnFor = ~(Uint64)0;        // what the crosshair surface shows, to redraw only on change
	Surface *_accuracyLine = nullptr;              // the accuracy line under the crosshair (see drawAccuracyLine)
	int _accuracyLineHalf = 0;                     // its widest reach to either side, in pixels (the view's half width)
	Uint64 _accuracyLineDrawnFor = ~(Uint64)0;     // what it shows, to redraw only on change
	Text *_txtMode = nullptr, *_txtShot = nullptr; // the fire mode, and the shot's odds and cost, beside the crosshair
	Text *_txtCosts = nullptr;                      // and under them its other costs (setting "Expanded crosshair info")
	Text *_txtDamage = nullptr;                     // and under the hit chance the shot's damage range (setting "Shot damage")
	Uint32 _hitMarkerHeard = 0;                    // the last hitmarker that ticked
	const BattleUnit *_sightCheckUnit = nullptr;   // the soldier the last line-of-sight check was for,
	Position _sightCheckFrom, _sightCheckTile;     // where he stood and the tile aimed at,
	int _sightCheckDirection = -1;                 // the way he faced,
	BattleActionType _sightCheckMode = BA_NONE;    // and the fire mode;
	Uint32 _sightCheckTime = 0;                    // when it was made (SDL ticks)
	bool _sightCheckResult = true;                 // and its answer

	/// Updates the crosshair and its texts: mode, hit chance, time units.
	void updateCrosshair(BattleUnit *unit, bool playerTurn);
	/// The game's own targeting's accuracy line: how far to either side a successful roll strays at what's under the crosshair, in pixels.
	int hitCloudReach(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, int accuracy, int &missReach);
	/// Draws the accuracy line: a level line as wide as the reach to either side, a notch up at each end.
	void drawAccuracyLine(int reach, int missReach, Uint8 color, Uint8 missColor, Uint8 shadow);
	/// A mod's own name for an action of an item, in capitals for the crosshair; empty if it keeps the game's name.
	std::string renamedAction(const RuleItem *rules, BattleActionType mode) const;
	/// Does the soldier have a line of sight to the aimed tile, as a shot checks it?
	bool aimedTileInSight(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, const Position &tile);
	/// Draws the crosshair for a fire mode.
	void drawCrosshair(BattleActionType mode, int shots, Uint8 color, Uint8 shadow, int hitMark, Uint8 hitColor);

	// ---- what the player points at: the unit's name under the crosshair, and the middle click ----

	Text *_txtTarget = nullptr;                    // the name of the unit pointed at, under the crosshair (setting "Unit names")
	BattleUnit *_pointedUnit = nullptr;            // the unit pointed at, found every frame (see updatePointedUnit)
	/// Where the player points on the view: the crosshair (its middle), or the free cursor.
	void pointer(int &pointerX, int &pointerY) const;
	/// Finds the unit the player points at, every frame: the one whose hitbox the game's line meets, else the one whose picture is there.
	void updatePointedUnit(BattleUnit *unit);
	/// The unit the player points at (see updatePointedUnit), or null.
	BattleUnit *pointedUnit() const { return _pointedUnit; }
	/// What the game's unit screen calls a unit.
	std::string unitLabel(const BattleUnit *target) const;
	/// Names the unit pointed at under the crosshair, and tells the view where the player points.
	void updateUnitName(BattleUnit *unit);
	/// The middle click as on the isometric map (setting "Middle click identifies"): shows the unit pointed at.
	bool identifyPointedUnit();

	// ---- guided missiles, psi attacks and the mind probe can be aimed on the isometric map ----

	enum MapAimPhase { MAPAIM_NONE, MAPAIM_AIMING, MAPAIM_ACTING };
	MapAimPhase _mapAimPhase = MAPAIM_NONE; // where an action aimed on the map is
	BattleActionType _mapAimType = BA_NONE; // which action that is

	/// Aims an action on the isometric map (missile waypoints, psi on an unseen creature).
	void startMapAim(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, const char *hint);
	/// Follows an action aimed on the isometric map; back to 3D when it's done.
	void updateMapAim();
	/// Is there a creature the player can see at this point?
	bool creatureAt(const Position &voxel, const BattleUnit *self) const;

	// =====================================================================================
	// The weapon in hand, bottom right (FirstPersonModeWeaponInHand.cpp)
	// =====================================================================================

	Surface *_weaponInHand = nullptr, *_heldSprite = nullptr; // where it's drawn; the held item's inventory picture (32x48)
	const BattleItem *_heldItem = nullptr;              // the item drawn in hand
	const RuleItem *_heldRules = nullptr;               // its rules
	int _heldClipId = -1;                               // the clip loaded last frame (-1: none; another one now: a reload)
	Uint32 _heldDipStart = 0, _muzzleFlashUntil = 0;    // when the weapon dipped out of view (reload, another weapon); until when the flash shows
	// the muzzle flash by what the weapon fires: colours, how many rays and how sharp, how big
	struct MuzzleFlash { Uint8 core, mid, rim; int rays; double spike, sharpness, size; };
	MuzzleFlash _muzzleFlash = {}; // the weapon's muzzle flash
	Uint32 _heldShotSeen = 0;      // our last shot the weapon flashed for
	// the weapon's inventory picture, measured: its long axis, which end is the muzzle, which side is up
	struct WeaponShape { double mean[2], along[2], across[2], stock, muzzle, minAcross, maxAcross, bore, boxMin[2], boxMax[2]; int upSign; bool gun, melee; }; // bore: across, where the barrel ends; box: the opaque pixels' bounds; melee: held tip up, swung down
	WeaponShape _heldShape = {};                        // the measured shape of the weapon's picture
	// ...and given volume: voxels, each picture pixel as deep as its distance from the outline
	// allows (a cushion), as the faces that show (see FirstPersonVoxelModel.h)
	std::vector<VoxelFace> _heldFaces; // the voxel faces of the weapon (or other item) in hand
	// a melee swing: wound up, struck in towards the crosshair, back; the view lunges with it
	Uint32 _swingStart = 0;
	bool _swingJolted = false; // the view has lunged for this swing already
	// dual wield (option): the gun in the other hand, drawn on the other side (its picture, shape
	// and voxels, swapped in while it's drawn), and its flash when its round leaves
	Surface *_offHandSprite = nullptr;
	const RuleItem *_offHandRules = nullptr; // the other gun's rules
	WeaponShape _offHandShape = {};          // its measured shape
	std::vector<VoxelFace> _offHandFaces;    // its voxel faces
	MuzzleFlash _offHandFlash = {};          // its muzzle flash
	Uint32 _offHandFlashUntil = 0;           // until when that flash shows
	Uint32 _offHandShotSeen = 0;             // its last round the flash was shown for

	/// Makes the surface the weapon in hand is drawn on.
	void makeWeaponInHand();
	/// Makes the scope cross's surface (canvas-sized, just above the weapon in hand) and draws the cross.
	void makeScopeCross();
	/// Shows the scope cross and the SCOPING line while zoomed in, with the zoom now.
	void updateScope();
	/// Draws the weapon in the soldier's hands at the bottom of the view.
	void updateWeaponInHand(BattleUnit *unit);
	/// The motion scanner held up, every frame: put away when it's no longer in hand, else drawn turned to the view.
	void updateScanner(BattleUnit *unit);
	/// Is this weapon drawn in the hands? Not a built-in one with nothing to hold (see the definition).
	bool weaponShownInHand(BattleUnit *unit, const BattleItem *weapon) const;
	/// Measures the weapon's picture: its long axis, muzzle end and up side.
	void analyzeWeapon(bool gun, bool melee = false);
	/// Draws an item's inventory picture whole into a 32x48 picture (not shifted as the hand slot does).
	void drawHeldPicture(const BattleItem *item, const SurfaceSet *inventorySprites, Surface *picture) const;
	/// Gives the held picture (weapon or other item) volume as voxels.
	void voxelizeWeapon();
	/// How the muzzle flashes, by the damage the weapon's ammo does.
	MuzzleFlash muzzleFlashFor(const BattleItem *weapon, const RuleItem *firedAmmo) const;
	/// Draws the weapon as its picture on a plate in perspective.
	void drawWeaponPlate(bool left, double bob[2], double kick, double dip, int shade, double tip[2]);
	/// Draws the weapon as voxels, placed like the plate.
	void drawWeaponVoxels(const double nearEnd[3], const double along[3], const double up[3], const double normal[3], double perPixel, int shade);
	/// Draws the held voxel faces placed by a map from picture space to camera space.
	void drawVoxelFaces(const double origin[3], const double stepPerX[3], const double stepPerY[3], const double stepPerDepth[3], int shade);
	/// Fills a convex four-sided shape on the weapon surface.
	void fillQuad(const double corners[4][2], Uint8 color);
	/// Draws a held item that isn't a weapon (grenade, medi-kit...), as voxels held up in the corner.
	void drawHeldItem(bool left, double bob[2], double dip, int shade);
	/// Dual wield: swaps the other hand's gun in for drawing, and back.
	void swapOffHand();
	/// Where a melee swing is: winding up, then striking; false if none.
	bool swingPhase(double &windup, double &strike) const;

	// =====================================================================================
	// The HUD: top bar, squad buttons, info panel, soldier card, banner (FirstPersonModeHud.cpp)
	// =====================================================================================

	// ---- the top bar ----

	TextButton *_btnView = nullptr, *_btnHud = nullptr, *_btnInventory = nullptr, *_btnEndTurn = nullptr; // ISO/3D, HUD, INV, END
	TextButton *_hudButtonGroup = nullptr, *_squadButtonGroup = nullptr; // button groups: the pressed look of HUD, and of the selected soldier
	TextButton *_btnPrev = nullptr, *_btnNext = nullptr; // previous / next soldier
	// the armor: its name, and buttons for what it and the gear can do (flight, personal light,
	// night vision, a special weapon), left of END
	Text *_txtArmor = nullptr;
	TextButton *_btnFly = nullptr, *_btnLight = nullptr, *_btnNight = nullptr, *_btnSpecial = nullptr; // FLY (SWIM), LIGHT, NIGHT VISION, SPECIAL
	TextButton *_flyButtonGroup = nullptr, *_lightButtonGroup = nullptr, *_nightButtonGroup = nullptr; // their pressed looks (switched on)
	std::string _suitLabelsLong[4], _suitLabelsShort[4]; // suit button labels: in full, and for a narrow bar

	// ---- one button per soldier ----

	std::vector<TextButton*> _btnSquad;   // one button per squad slot
	std::vector<BattleUnit*> _squadUnits; // unit shown by each squad button
	// how each squad member is: an icon replaces the number of the fallen and the turned
	enum SquadState { SQUAD_NONE, SQUAD_READY, SQUAD_DEAD, SQUAD_UNCONSCIOUS, SQUAD_CONTROLLED, SQUAD_ZOMBIE };
	std::vector<SquadState> _squadStates;         // how each squad member is
	std::vector<Surface*> _squadIcons;            // the icon on each button (for the fallen and the turned)
	std::vector<size_t> _squadSlots;              // the slots with a button, in order
	Uint32 _pelletSoundsFrom = 0; int _pelletSounds = 0; // pellets heard in the last moment (see pelletLanded): when it began, how many
	int _squadScroll = 0, _squadWindow = 10;      // the buttons shown at once (ten, more on a wide screen); the rest scroll
	const BattleUnit *_squadScrolledTo = nullptr; // the selected soldier the window last followed
	Uint8 _squadColor = 0;                        // the buttons' own colour

	// ---- the info panel, bottom left ----

	Text *_txtName = nullptr, *_txtDoom = nullptr, *_txtWeapon = nullptr, *_txtStatus = nullptr; // name, DOOM MODE / IDDQD / IDKFA (cheats), weapon, status
	Text *_txtWeaponAmmo = nullptr;                     // the weapon's rounds and spare clips, right after its name
	Text *_txtOtherWeapon = nullptr, *_txtOtherAmmo = nullptr; // the other hand's item, and its rounds and spare clips
	Uint8 _weaponTextColor = 0;                         // the weapon lines' own colour (the mod's "weapon" element)
	NumberText *_numTimeUnits = nullptr, *_numEnergy = nullptr, *_numHealth = nullptr, *_numMorale = nullptr; // the four stats
	Bar *_barTimeUnits = nullptr, *_barEnergy = nullptr, *_barHealth = nullptr, *_barMorale = nullptr; // and their bars
	Uint8 _statColors[8] = {};                          // the stats' own colours (numbers, then bars), for when they stop being silver
	bool _statsSilver = false;                          // the stats are silver: Doom IDDQD, an invincible soldier
	std::vector<Surface*> _panelParts;                  // all of the panel's surfaces, shown and hidden together

	// ---- the soldier card: hovering a squad button shows the soldier, and (an option) their
	// helmet camera ----

	struct CardPart { Surface *surface; int dx, dy; };
	std::vector<CardPart> _cardParts;                   // the card's surfaces, and where each sits on it
	Surface *_card = nullptr;                           // the card's frame
	FirstPersonView *_preview = nullptr;                // the live view from the soldier's helmet camera
	Text *_cardName = nullptr, *_cardWeapon = nullptr, *_cardStatus = nullptr, *_cardSignal = nullptr; // name, weapon, status, and SIGNAL LOST
	Surface *_panelRank = nullptr, *_cardRank = nullptr, *_soldierCamRank = nullptr; // a soldier's rank before their name: the panel's, the card's, the soldier cam's title
	int _panelNameX = 0;                                // where the panel's name line starts (the rank, then the name)
	std::unordered_map<Surface*, int> _rankDrawn;       // the rank picture each of those holds (-1: none), so it's drawn only when it changes
	std::map<int, std::unique_ptr<Surface>> _smallRankBadges; // the battlescape rank badges made small (see smallRankBadge), by sprite
	std::map<int, std::unique_ptr<Surface>> _craftIcons;     // the craft icons in the battlescape's colours (see craftIcon), by sprite (a tank's plus CraftIconItemKey)
	const Surface *_cardIconDrawn = nullptr;                  // the craft icon the card's icon holds, so it's drawn only when it changes
	Surface *_cardIcon = nullptr;                             // the soldier card's craft icon (setting "Craft icons"), before the name
	Uint32 _cardNoiseSeed = 1;                          // random seed for the card's TV static
	NumberText *_cardNumbers[4] = {};                   // the four stats
	Bar *_cardBars[4] = {};                             // and their bars
	int _hoveredSquadButton = -1;                       // squad button whose card shows: the one under the cursor (or the wheel's, see browseSquad), -1 none
	const TextButton *_buttonUnderCursor = nullptr;     // the button the free cursor was last found over
	int _cursorX = 0, _cursorY = 0;                     // where the free cursor is, on the canvas (and so on the view)
	int _cardDrawnHeight = 0;                           // what the frame was drawn for

	// ---- the soldier cam: a right click on a squad button enlarges its card's camera ----

	bool _soldierCamOpen = false;           // the soldier cam is open (a right click on a squad button; closes off the bar, or with another)
	FirstPersonView *_soldierCam = nullptr; // its picture: the helmet camera, big
	Surface *_soldierCamFrame = nullptr;    // its frame, with a title bar
	Surface *_soldierCamDot = nullptr;      // the red dot of a live feed, blinking
	Surface *_mapCameraDot = nullptr;       // the map camera's, the same
	Text *_txtSoldierCam = nullptr;         // "SOLDIER CAM: <name>"
	int _soldierCamWidth = 0, _soldierCamHeight = 0; // the picture's size, in canvas pixels
	/// Points a helmet camera (the card's, the soldier cam, the map camera) through a soldier's eyes, the way they face, at a unit or at a point.
	void aimHelmetCamera(FirstPersonView *camera, BattleUnit *unit, const BattleUnit *lookAt = nullptr, const double *lookAtPoint = nullptr);
	/// Shows the soldier cam for a soldier with its frame's left edge at a canvas x, or hides it (null).
	void updateSoldierCam(BattleUnit *unit, int frameX);

	// ---- the map camera: on the isometric map, the selected soldier's helmet camera in a window ----

	FirstPersonView *_mapCamera = nullptr;  // its picture
	Surface *_mapCameraFrame = nullptr;     // its frame, with a title line
	Text *_txtMapCamera = nullptr;          // the title: the soldier's name
	int _mapCameraSize = 0;                 // the size the picture was made at (see makeMapCamera)
	int _mapCameraCanvasWidth = 0;          // and the canvas's width it was made for
	bool _mapSoldierCamOpen = false;        // the soldier cam opened from it (a right click; closes off it, or with another)
	/// Shows the map camera (or the soldier cam opened from it) in the player's turn on the map, or hides it.
	void updateMapCamera();
	/// Is a point on the canvas on the map camera, or on the soldier cam opened from it?
	bool onMapCamera(int canvasX, int canvasY) const;
	/// The 3D HUD key on the map: the map camera off, or on again at its last size.
	void toggleMapCamera();

	// ---- the banner across the top of the view ----

	Text *_txtBanner = nullptr;     // the banner (big letters)
	std::vector<Text*> _bannerGlow; // the banner again around itself, glowing

	/// Does this unit have night vision (gear, or eyes that see in the dark)?
	bool hasNightVision(const BattleUnit *unit) const;
	/// Switches night vision on or off for a soldier with the gear, whatever the game is doing.
	void toggleNightVision(BattleUnit *unit);
	/// Fills the info panel for a unit (or hides it).
	void updateInfoPanel(BattleUnit *unit);
	/// The soldier card's weapon line: "R: Rifle 17 | 2 clips (40)".
	std::string weaponLabel(BattleUnit *unit, int *tagColor = nullptr, int lessRoom = 0) const;
	/// A soldier's (or tank's) little figure from the craft screen, in the battlescape's colours; null for none.
	Surface *craftIcon(const BattleUnit *unit);
	/// A gun's rounds for a fire mode and the spare clips carried for it: "17 | 2 clips (40)".
	std::string ammoLabel(const BattleUnit *unit, const BattleItem *weapon, BattleActionType mode, bool &loaded, int *tagColor = nullptr) const;
	/// The loaded clip's damage type in two letters, for a gun taking several: "HE"; empty for none.
	std::string ammoTag(const BattleItem *weapon, int slot, const BattleItem *clip, Uint8 &color) const;
	/// Fills one hand's line of the info panel: the item's name and, for a gun, its ammo.
	void fillHandLine(Text *nameText, Text *ammoText, BattleUnit *unit, BattleItem *item, BattleActionType mode, bool active);
	/// Shortens a text (ending it in ".") until it fits a width in the small font.
	std::string fitText(const std::string &text, int maxWidth) const;
	/// Width of a text in the small font, in pixels.
	int smallTextWidth(const std::string &text) const;
	/// The status line: breakdown, on fire, wounds, or kneeling.
	std::string statusLabel(BattleUnit *unit) const;
	/// The armor's name and its buttons (fly, light, night vision, special).
	void updateArmorButtons(BattleUnit *unit);
	/// Shows the soldier card for the hovered squad button.
	void updateSoldierCard();
	/// The banner across the top: hidden movement, enemy spotted, burning.
	void updateBanner(bool playerTurn);
	/// Works out which unit each squad button stands for.
	void refreshSquad();
	/// Lays out the squad buttons that are in use.
	void layoutTopBar();
	/// Scrolls the squad buttons.
	void scrollSquad(int step);
	/// Moves the soldier card (and soldier cam) on to the next or previous soldier: the wheel over the squad bar.
	void browseSquad(int step, double cursorX, double cursorY);
	/// How a squad member is: ready, dead, unconscious, controlled, zombie.
	SquadState squadState(const BattleUnit *unit) const;
	/// Draws the icon for a fallen or turned squad member.
	void drawSquadIcon(Surface *icon, SquadState state);
	/// Keeps the top bar in sync with the game.
	void updateTopBar(BattleUnit *selected);
	/// Which of our buttons is at this canvas point?
	TextButton *buttonAt(double x, double y) const;
	/// Acts on a click on one of our buttons.
	void buttonClicked(TextButton *button, Action *action);
	/// Selects a soldier from the squad bar.
	void selectUnit(BattleUnit *unit);
	/// Hands the 3D view the visible enemies to mark.
	void updateEnemyMarkers();

	// =====================================================================================
	// What happens to the soldier: hurt, burning, down, breakdowns
	// (FirstPersonModeSoldierStates.cpp)
	// =====================================================================================

	// ---- getting hurt ----

	const BattleUnit *_healthWatchedUnit = nullptr; // the unit whose health we watch
	int _lastHealth = 0;                            // its health last frame
	Uint32 _hurtStart = 0;                          // when the last red flash started
	float _hurtAmount = 0.0f;                       // how strong it is (0..1)
	int _lastHeartbeat = -1;                        // the last heartbeat that thumped
	Uint32 _nearMissSeen = 0;                       // the last near miss we reacted to

	/// Flashes red when the soldier we look through is hurt.
	void checkHurt(BattleUnit *unit, Uint32 now);
	/// The purple haze at the view's edges for a unit under our mind control, breathing (0 for any other).
	float controlHaze(const BattleUnit *unit, Uint32 now) const;
	/// Reacts to an enemy shot whizzing past close by.
	void checkNearMiss();

	// ---- on fire: the view in flames, a flare-up and a banner when the soldier catches fire ----

	const BattleUnit *_burningWatchedUnit = nullptr; // the unit whose fire we watch
	int _lastFireTurns = 0;                          // the turns of fire it had last frame (more now: it caught fire)
	Uint32 _burningBannerUntil = 0;                  // until when the BURNING banner shows

	/// Shows the soldier on fire, and catching fire.
	void checkBurning(BattleUnit *unit, Uint32 now);

	// ---- going down (killed, unconscious, taken over, turned into a zombie): the view stays
	// with the soldier a moment ----

	enum FallKind { FALL_KILLED, FALL_UNCONSCIOUS, FALL_CONTROLLED, FALL_PANIC, FALL_BERSERK, FALL_ZOMBIE };
	BattleUnit *_deathUnit = nullptr;      // the soldier who went down (null: none)
	Uint32 _deathStart = 0;                // when the view started showing it
	double _deathEyeZ = 0.0;               // the eye height they fell from (a zombie gets up to about it again)
	FallKind _fallKind = FALL_KILLED;      // what happened to them
	bool _allSoldiersLost = false;         // that was the last soldier standing
	Text *_txtFallName = nullptr;          // whose camera it was, under the banner
	Uint32 _afterFallStart = 0;            // the next soldier's view coming in out of the static or the colour
	FallKind _afterFallKind = FALL_KILLED; // what the next soldier's view comes in out of
	Uint32 _afterFallTime = 0;             // ...over this long

	/// Stays with a soldier who went down a moment; true while it's showing.
	bool deathCam(Uint32 now, double elapsedSeconds);

	// ---- setting "Quick deaths": the soldier looked through going down falls on their helmet
	// camera card while the view goes on to the next soldier at once ----

	const BattleUnit *_deathFeedUnit = nullptr;  // the soldier whose fall plays on the card (null: none)
	Uint32 _deathFeedStart = 0;                  // when they went down
	FallKind _deathFeedKind = FALL_KILLED;       // killed or knocked out
	double _deathFeedEye[3] = {};                // the eye they fell from, in voxels...
	double _deathFeedYaw = 0.0;                  // ...the way they looked...
	double _deathFeedPitch = 0.0;                // ...and how far up or down
	BattleUnit *_quickNextSoldier = nullptr;     // the soldier the view went on to (selected once the game is free; null: none)

	/// Hands the view over at once: the fall goes on the soldier's card, the view to the next soldier.
	void quickHandOver(BattleUnit *unit, Uint32 now);
	/// The next soldier of ours standing after this one, in the squad's order (null: none).
	BattleUnit *nextStandingSoldier(const BattleUnit *after) const;
	/// Points the card's camera through the falling soldier's eyes, at this moment of their fall.
	void aimDeathFeed(FirstPersonView *camera, Uint32 now);

	// ---- the aliens' turn cameras (setting "Aliens' turn cameras"): the view cuts to the soldier
	// who sees the alien acting (the nearest), or to one of ours firing back; a soldier shot at,
	// hit or taken over pops up a small camera (the soldier card's). With the aliens' turn on
	// the map, the soldier the view would cut to pops up a camera too ----

	BattleUnit *_alienTurnCamera = nullptr;               // whose eyes the view looks through on the aliens' turn (null: the last soldier)
	bool _alienTurnCamerasRan = false;                    // they ran this aliens' turn (the player's turn then starts with the view afresh)
	Uint32 _alienTurnCameraSince = 0;                     // when the view cut to them
	const BattleUnit *_alienTurnWatched = nullptr;        // the alien the view last turned to...
	Position _alienTurnWatchedTile = Position(-1, -1, -1); // ...where it stood then (the view turns to it again when it moves on)
	const BattleUnit *_alertUnit = nullptr;               // the soldier the popped-up camera shows
	const BattleUnit *_alertLookAt = nullptr;             // the unit it looks at (null: the way the soldier faces)
	Uint32 _alertSince = 0;                               // since when (SDL ticks)
	Uint32 _alertUntil = 0;                               // until when
	std::string _alertText;                               // why ("TARGETED", "HIT") as a string id, shown as their status; empty: their own status
	std::unordered_map<const BattleUnit*, std::pair<int, int>> _alertWatch; // each soldier of ours last frame: health less stun, and side (-1: down)
	/// The aliens' turn in 3D: whose eyes the view looks through (null: the last soldier), and the cameras that pop up.
	BattleUnit *alienTurnCamera(Uint32 now);
	/// The aliens' turn on the map: the cameras that pop up, among them whoever the 3D view would cut to.
	void alienTurnMapCameras(Uint32 now);
	/// Pops up the camera of a soldier of ours who was hit, taken over or went down since the last frame.
	void watchSquad(Uint32 now);
	/// The shot in flight: who fired it and at whom (null: none); a soldier of ours an alien shoots at pops up their camera.
	void watchShot(Uint32 now, BattleUnit *&shooter, BattleUnit *&shotAt);
	/// The soldier of ours nearest a unit who sees it (null: none).
	BattleUnit *nearestSpotter(const BattleUnit *unit) const;
	/// Cuts the aliens' turn view to a soldier, looking at a unit.
	void cutAlienTurnCamera(BattleUnit *soldier, const BattleUnit *lookAt, Uint32 now);
	/// Pops up the small camera on a soldier for a few seconds, with why (a string id; empty: their own status), looking at a unit.
	void popUpCamera(const BattleUnit *soldier, const std::string &why, Uint32 now, const BattleUnit *lookAt = nullptr);
	/// Puts up what happened to the soldier, and whose camera it is.
	void showFallBanner(FallKind kind, const std::string &title, const std::string &name);

	// ---- a soldier's breakdown at the start of the turn (panic, berserk): the view washes out
	// meanwhile ----

	BattleUnit *_breakdownUnit = nullptr;               // the soldier breaking down (null: none)
	bool _breakdownBerserk = false;                     // berserk (else panic)
	Uint32 _breakdownStart = 0;                         // when the view started showing it (0: not yet)
	bool _breakdownShot = false;                        // a shot was in the air last frame (each new one flashes red)
	Uint32 _breakdownFlash = 0;                         // when the last red flash (a berserk shot) started
	std::unordered_map<const BattleUnit*, std::pair<int, bool>> _breakdownTurns; // who broke down in which turn, berserk or not (their camera says so)

	/// Shows a soldier's breakdown through their eyes; true while it's showing.
	bool breakdownCam(Uint32 now, double elapsedSeconds);
	/// Takes a breakdown's banner down, and forgets it (or keeps it to show again).
	void endBreakdown(bool forget = true);
};

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
