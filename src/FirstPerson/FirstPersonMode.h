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
	/// Should this shot's bullet hits play quicker (an FPS-style shot of ours)?
	static bool fastShots(BattlescapeState *state, const BattleUnit *attacker);
	/// A soldier panics or goes berserk, asked by the game (see FirstPerson::breakdown).
	static bool breakdown(BattlescapeState *state, BattleUnit *unit);
	/// Doom IDKFA: does this unit's shot spend no ammo? Asked by the game (see FirstPerson::endlessAmmo).
	static bool endlessAmmo(BattlescapeState *state, const BattleUnit *unit);
	/// Doom IDDQD: does this unit take no damage? Asked by the game (see FirstPerson::invincible).
	static bool invincible(BattlescapeState *state, const BattleUnit *unit);
	/// Where the next round of an FPS-style shot goes: along the crosshair, with a spread.
	void freeAimTarget(const BattleAction &action, Position &target);

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
	bool _classicHud = false;              // original bottom HUD drawn over the 3D view
	bool _hudHidden = false;               // the 3D HUD hidden (the hide HUD key): a clear view
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
	/// Makes the top bar's buttons: 3D, HUD, INV, previous / next soldier, the suit's buttons, END.
	void createTopBar();
	/// Makes the info panel, bottom left: name, weapon, status, the four stats.
	void createInfoPanel();
	/// Makes the soldier card shown over a squad button, with its live helmet-camera view.
	void createSoldierCard();
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
	/// Notes the player's choices (3D, classic HUD) for a remade screen of the same battle.
	void rememberChoices() const;
	/// Shows or hides the original bottom HUD over the 3D view.
	void setClassicHud(bool on);
	/// Hides or shows the 3D HUD (the hide HUD key).
	void toggleHud();
	/// Is the 3D HUD hidden right now (in the 3D view, with the hide HUD key)?
	bool hudHidden() const;
	/// Switches between mouse look (captured mouse) and a free cursor.
	void setMouseLook(bool on);
	/// Can the player act right now (their turn, nothing else going on)?
	bool canAct() const;
	/// Is doom mode on, by itself or with one of its extras (IDKFA, IDDQD)?
	static bool doomModeOn();
	/// The doom mode key: all the cheats off, or back on as they were.
	void toggleCheats();
	/// Doom mode (debug): refills the soldier's time units and energy (and ammo, with IDKFA).
	void applyDoomMode(BattleUnit *unit);
	/// Doom IDDQD: keeps X-COM's units at full morale.
	void applyGodMode();
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
	Text *_compassLetters[4] = {};               // N, E, S, W, going round as the view turns
	Text *_txtCompassTurn = nullptr;             // under it near the head's limit: "TURN 1 TU", or "NO TU"; or a shot's turn: "SHOT +2 TU"
	int _compassDrawn[5] = { -1000, -1, -1, -1, -1 }; // what the dial shows: the view's bearing (degrees), the facing, the warning, the shot's facing, the shot's turn warning

	/// Shows the compass, redrawn when what it shows changes.
	void updateCompass(BattleUnit *unit, bool playerTurn);
	/// Draws the dial for a view bearing (degrees, 0 north, clockwise), a facing (0..7), a warning (0 none, 1 near the limit, 2 no time units to turn) and the facing a shot will turn to (-1 none).
	void drawCompass(int viewBearing, int facing, int warning, int shotFacing);
	/// The time units the game's turn before a shot or throw at what's under the crosshair will cost, and the facing it turns to.
	int shotTurnCost(BattleUnit *unit, int &direction, bool &affordable) const;
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
	int _stepAllowedCache[8];                  // per frame: -1 unknown, 0 blocked, 1 allowed
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
	/// Opens the door the soldier faces without walking through it.
	void openDoor(BattleUnit *unit);

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

	/// Shows the lift arrows while the soldier stands on a grav lift.
	void updateLift(BattleUnit *unit, Uint32 now);
	/// Draws a pulsing purple lift arrow, up or down.
	void drawLiftArrow(Surface *surface, bool up, bool hover, Uint32 now);
	/// Which lift arrow is at this canvas point: 1 up, -1 down, 0 none.
	int liftArrowAt(double x, double y) const;

	// ---- flying and falling ----

	std::unordered_map<const BattleUnit*, bool> _flightSwitchedOn; // flight switched on, per soldier
	bool _landing = false;                              // flight switched off in the air: coming down a level at a time
	bool _landAfterStep = false;                        // flight off, stepping off a ramp's edge: land once there
	int _fallFromZ = -1;                                // a step off a ledge: the level it was taken from (fall damage on landing)

	/// Can this unit fly (or swim) at all?
	bool canFly(const BattleUnit *unit) const;
	/// Is flight switched on for this soldier?
	bool flightOn(const BattleUnit *unit) const;
	/// Switches flight on or off.
	void toggleFlight(BattleUnit *unit);
	/// Goes up or down a level: by grav lift, or flying.
	void moveUpDown(BattleUnit *unit, int direction, Action *action);
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
	Uint32 _tileMoveKeysSince = 0;       // when the movement keys were pressed (a moment's wait catches a second key, for a diagonal)
	double _tileMoveClosest = 0.0;       // the closest the camera has come to the target's middle, voxels
	Uint32 _tileMoveProgressAt = 0;      // when it last came closer

	/// The movement keys' push: forward (1) or back (-1), right (1) or left (-1).
	void movementKeys(double &forward, double &strafe) const;
	/// The numpad's push for a move (8 ahead, 2 back, 4 left, 6 right), held right now.
	void numpadKeys(int &forward, int &strafe) const;
	/// Tile moves: the velocity that walks the camera to the next tile's middle; starts and ends the moves.
	bool tileMoveVelocity(BattleUnit *unit, double forward, double strafe, bool canMove, double walkSpeed, double elapsedSeconds, double &velocityX, double &velocityY);
	Uint32 _spottedUntil = 0;             // "enemy spotted" banner

	/// Stops the walk when a new enemy comes into view.
	void checkSpotted(BattleUnit *unit, bool playerTurn);

	// =====================================================================================
	// Weapons, fire modes, aiming and firing (FirstPersonModeShooting.cpp)
	// =====================================================================================

	std::unordered_map<const BattleUnit*, int> _chosenHand; // each soldier's hand picked with G (or on the classic HUD): 0 right, 1 left, 2 built-in weapon
	struct DoomFuse { Uint32 lit, length; };                // a real-time fuse: when it started burning (0: not yet, the grenade is still in hand), and for how long (ms)
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
	bool _akimboRealisticWasOn = true;                      // the akimbo key switches realistic akimbo back on (false: full akimbo)
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
	/// Akimbo: the gun in the other hand that fires along, or null.
	BattleItem *offHandWeapon(BattleUnit *unit, const BattleItem *weapon) const;
	/// The akimbo key: full or realistic akimbo off, or the same one back on.
	void toggleAkimbo();
	/// Dual wield: the other hand's gun if it can fire along with this mode, and the mode it fires.
	BattleItem *dualPartner(BattleUnit *unit, const BattleItem *weapon, BattleActionType slot, BattleActionType &partnerMode) const;
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
	/// Notes what's under the crosshair this frame.
	void updateAim(BattleUnit *unit);
	/// The first voxel along the ray through a pixel, as a shot would see it; true if something is there.
	bool aimAt(int pixelX, int pixelY, BattleUnit *unit, Position &voxel);
	/// Uses the fire mode on whatever is under a pixel of the view, through the game's own actions.
	void fire(int pixelX, int pixelY);
	/// The patient a medi-kit would treat now: among the game's own choices, the one pointed at; or null.
	BattleUnit *medikitPatient(BattleUnit *unit, BattleItem *medikit) const;
	/// Uses a medi-kit (no action menu) on the patient pointed at, or a scanner, as the game's menu would.
	void useItemDirectly(BattleUnit *unit, BattleItem *item);
	/// Tosses a tactical flare at the crosshair.
	void throwFlare();

	// ---- FPS-style combat (setting "Faithful X-COM targeting" off): rounds go along the
	// crosshair with a spread, recoil lifts the aim, holding the trigger in auto keeps firing ----

	bool _triggerHeld = false;                   // the left button is held down (auto keeps firing)
	Uint32 _recoilShotSeen = 0, _lastRecoil = 0; // our last shot the recoil reacted to; when the view was last kicked up
	double _recoilDebt = 0.0;                    // degrees the recoil lifted the view, eased back after firing
	double _bloom = 0.0;                         // extra spread from the last rounds (degrees, 95% circle)

	/// Is this fire mode FPS style just now?
	bool freeAimOn(BattleActionType mode) const;
	/// What a fire mode actually fires (FPS style: the quick slot is a burst).
	BattleActionType firedAs(const BattleItem *weapon, BattleActionType mode) const;
	/// How wide an FPS-style shot spreads, in degrees (95% of rounds).
	double spreadFor(const BattleUnit *unit, BattleActionType mode, bool dual = false) const;
	/// FPS-style recoil: lifts the view with each round, then eases it back.
	void updateRecoil(BattleUnit *unit, double elapsedSeconds);
	/// FPS-style auto: fires the next burst while the trigger is held.
	void keepFiringWhileHeld(BattleUnit *unit, bool playerTurn);

	// ---- the crosshair, with the fire mode and the shot's odds and cost under it ----

	Surface *_crosshair = nullptr;                 // the crosshair's surface
	Uint32 _crosshairDrawnFor = 0xFFFFFFFF;        // what the crosshair surface shows, to redraw only on change
	Text *_txtMode = nullptr, *_txtShot = nullptr; // the fire mode, and the shot's odds and cost, beside the crosshair
	Uint32 _hitMarkerHeard = 0;                    // the last hitmarker that ticked
	const BattleUnit *_sightCheckUnit = nullptr;   // the soldier the last line-of-sight check was for,
	Position _sightCheckFrom, _sightCheckTile;     // where he stood and the tile aimed at,
	int _sightCheckDirection = -1;                 // the way he faced,
	BattleActionType _sightCheckMode = BA_NONE;    // and the fire mode;
	Uint32 _sightCheckTime = 0;                    // when it was made (SDL ticks)
	bool _sightCheckResult = true;                 // and its answer

	/// Updates the crosshair and its texts: mode, hit chance, time units.
	void updateCrosshair(BattleUnit *unit, bool playerTurn);
	/// Does the soldier have a line of sight to the aimed tile, as a shot checks it?
	bool aimedTileInSight(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, const Position &tile);
	/// Draws the crosshair for a fire mode.
	void drawCrosshair(BattleActionType mode, int shots, Uint8 color, Uint8 shadow, int hitMark, Uint8 hitColor);

	// ---- what the player points at: the unit's name under the crosshair, and the middle click ----

	Text *_txtTarget = nullptr;                    // the name of the unit pointed at, under the crosshair (setting "Unit names")
	/// Where the player points on the view: the crosshair (its middle), or the free cursor.
	void pointer(int &pointerX, int &pointerY) const;
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
	/// Is this weapon drawn in the hands? Not a built-in one with nothing to hold (see the definition).
	bool weaponShownInHand(BattleUnit *unit, const BattleItem *weapon) const;
	/// Measures the weapon's picture: its long axis, muzzle end and up side.
	void analyzeWeapon(bool gun, bool melee = false);
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
	int _squadScroll = 0, _squadWindow = 10;      // at most ten buttons at once; the rest scroll
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
	Uint32 _cardNoiseSeed = 1;                          // random seed for the card's TV static
	NumberText *_cardNumbers[4] = {};                   // the four stats
	Bar *_cardBars[4] = {};                             // and their bars
	int _hoveredSquadButton = -1;                       // squad button under the cursor, -1 none
	int _cursorX = 0, _cursorY = 0;                     // where the free cursor is, on the canvas (and so on the view)
	int _cardDrawnHeight = 0;                           // what the frame was drawn for

	// ---- the banner across the top of the view ----

	Text *_txtBanner = nullptr;     // the banner (big letters)
	std::vector<Text*> _bannerGlow; // the banner again around itself, glowing

	/// Does this unit have night vision (gear, or eyes that see in the dark)?
	bool hasNightVision(const BattleUnit *unit) const;
	/// Switches night vision on or off for a soldier with the gear.
	void toggleNightVision(BattleUnit *unit, Action *action);
	/// Fills the info panel for a unit (or hides it).
	void updateInfoPanel(BattleUnit *unit);
	/// The soldier card's weapon line: "R: Rifle 17 | 2 clips (40)".
	std::string weaponLabel(BattleUnit *unit) const;
	/// A gun's rounds for a fire mode and the spare clips carried for it: "17 | 2 clips (40)".
	std::string ammoLabel(const BattleUnit *unit, const BattleItem *weapon, BattleActionType mode, bool &loaded) const;
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

	// ---- the aliens' turn cameras (setting "Aliens' turn cameras"): the view cuts to the soldier
	// who sees the alien acting (the nearest), or to one of ours firing back; a soldier shot at,
	// hit or taken over pops up a small camera (the soldier card's) ----

	BattleUnit *_alienTurnCamera = nullptr;               // whose eyes the view looks through on the aliens' turn (null: the last soldier)
	bool _alienTurnCamerasRan = false;                    // they ran this aliens' turn (the player's turn then starts with the view afresh)
	Uint32 _alienTurnCameraSince = 0;                     // when the view cut to them
	const BattleUnit *_alienTurnWatched = nullptr;        // the alien the view last turned to...
	Position _alienTurnWatchedTile = Position(-1, -1, -1); // ...where it stood then (the view turns to it again when it moves on)
	const BattleUnit *_alertUnit = nullptr;               // the soldier the popped-up camera shows
	Uint32 _alertUntil = 0;                               // until when (SDL ticks)
	std::string _alertText;                               // why ("TARGETED", "HIT") as a string id, shown as their status; empty: their own status
	std::unordered_map<const BattleUnit*, std::pair<int, int>> _alertWatch; // each soldier of ours last frame: health less stun, and side (-1: down)
	/// The aliens' turn: whose eyes the view looks through (null: the last soldier), and the cameras that pop up.
	BattleUnit *alienTurnCamera(Uint32 now);
	/// Cuts the aliens' turn view to a soldier, looking at a unit.
	void cutAlienTurnCamera(BattleUnit *soldier, const BattleUnit *lookAt, Uint32 now);
	/// Pops up the small camera on a soldier for a few seconds, with why (a string id; empty: their own status).
	void popUpCamera(const BattleUnit *soldier, const std::string &why, Uint32 now);
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
