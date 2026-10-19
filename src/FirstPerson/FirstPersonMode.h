#pragma once
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
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <SDL.h>
#include "../Battlescape/Position.h"
#include "../Mod/RuleItem.h"

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
 */
class FirstPersonMode
{

	Game *_game;
	BattlescapeState *_state;
	SavedBattleGame *_save;
	FirstPersonView *_view;
	TextButton *_btnView, *_btnHud, *_btnInventory, *_btnEndTurn;
	TextButton *_hudGroup, *_squadGroup;
	// the armor: its name, and buttons for what it and the gear can do (flight, personal light,
	// night vision, a special weapon)
	Text *_txtArmor = nullptr;
	TextButton *_btnFly = nullptr, *_btnLight = nullptr, *_btnNight = nullptr, *_btnSpecial = nullptr; // top bar, left of END
	TextButton *_btnPrev = nullptr, *_btnNext = nullptr;                          // previous / next soldier
	TextButton *_flyGroup = nullptr, *_lightGroup = nullptr, *_nightGroup = nullptr;
	std::unordered_map<const BattleUnit*, bool> _flight; // flight switched on, per soldier
	bool _landing = false;     // flight switched off in the air: coming down a level at a time
	bool _landAfterStep = false; // flight off, stepping off a ramp's edge: land once there
	int _fallFromZ = -1;       // a step off a ledge: the level it was taken from (fall damage on landing)
	void fallDamage(BattleUnit *unit, int levels);
	// a grav lift (or ladder) under the soldier: purple arrows up and down with LIFT PLATFORM
	// between them; a click (free cursor) or the up/down keys ride it
	Surface *_liftUp = nullptr, *_liftDown = nullptr;
	Text *_txtLift = nullptr, *_txtLiftKeys = nullptr;
	int _liftHover = 0;        // 1 over the up arrow, -1 over the down arrow
	int _liftWays = -1;        // which arrows the keys line was written for (bit 1 up, bit 2 down)
	Uint32 _rideStart = 0;     // up or down a level where the soldier stands: the eye glides there
	Uint32 _rideTime = 0;      // how long this glide takes
	double _rideFrom = 0.0, _rideTo = 0.0; // eye heights; _rideTo is the estimate until the unit is up
	int _rideLevel = 0;        // the level the ride started from
	bool _rideVertical = false; // a vertical move (lift, flying) seen last frame
	void updateLift(BattleUnit *unit, Uint32 now);
	void drawLiftArrow(Surface *surface, bool up, bool hover, Uint32 now);
	int liftAt(double x, double y) const;
	std::vector<TextButton*> _btnSquad;
	std::vector<BattleUnit*> _squad;          // unit shown by each squad button
	// how each squad member is: an icon replaces the number of the fallen and the turned
	enum SquadState { SQUAD_NONE, SQUAD_READY, SQUAD_DEAD, SQUAD_UNCONSCIOUS, SQUAD_CONTROLLED, SQUAD_ZOMBIE };
	std::vector<SquadState> _squadStates;
	std::vector<Surface*> _squadIcons;
	std::vector<size_t> _squadSlots;          // the slots with a button, in order
	int _squadScroll = 0, _squadWindow = 10;  // at most ten buttons at once; the rest scroll
	const BattleUnit *_scrolledTo = nullptr;  // the selected soldier the window last followed
	Uint8 _squadColor = 0;                    // the buttons' own colour
	std::string _suitLong[4], _suitShort[4];  // suit button labels: in full, and for a narrow bar
	std::vector<Text*> _shadows;                        // dark copies behind our texts, so they read over the world
	std::vector<NumberText*> _numberShadows;
	Text *_txtName, *_txtDoom, *_txtWeapon, *_txtStatus;
	NumberText *_numTimeUnits, *_numEnergy, *_numHealth, *_numMorale;
	Bar *_barTimeUnits, *_barEnergy, *_barHealth, *_barMorale;
	std::vector<Surface*> _panelParts;
	// soldier card: hovering a squad button shows the soldier, and (an option) their helmet camera
	struct CardPart { Surface *surface; int dx, dy; };
	std::vector<CardPart> _cardParts;
	Surface *_card = nullptr;
	FirstPersonView *_preview = nullptr;
	Text *_cardName = nullptr, *_cardWeapon = nullptr, *_cardStatus = nullptr, *_cardSignal = nullptr;
	Uint32 _noiseSeed = 1;
	NumberText *_cardNumbers[4] = {};
	Bar *_cardBars[4] = {};
	int _hoverIndex = -1;   // squad button under the cursor, -1 none
	int _cardHeight = 0;    // what the frame was drawn for
	// the weapon in hand, bottom right (the game's hand sprite, doubled)
	Surface *_viewModel = nullptr, *_handSprite = nullptr;
	const BattleItem *_vmItem = nullptr;
	const RuleItem *_vmRules = nullptr;
	int _vmShade = -1, _vmAmmo = -1;
	Uint32 _vmDipStart = 0, _vmFlashUntil = 0;
	// the muzzle flash by what the weapon fires: colours, how many rays and how sharp, how big
	struct MuzzleFlash { Uint8 core, mid, rim; int rays; double spike, sharpness, size; };
	MuzzleFlash _vmFlash = {};
	MuzzleFlash flashFor(const BattleItem *weapon) const;
	Uint32 _vmShotSeen = 0;
	// a melee swing: wound up, struck in towards the crosshair, back; the view lunges with it
	Uint32 _vmSwingStart = 0;
	bool _swingJolted = false;
	bool swingPhase(double &windup, double &strike) const;
	Uint32 _nearMissSeen = 0;       // the last near miss we reacted to
	void checkNearMiss();
	// the weapon's inventory picture, measured: its long axis, which end is the muzzle, which side is up
	struct WeaponShape { double mean[2], along[2], across[2], stock, muzzle, minAcross, maxAcross, bore; int upSign; bool gun; }; // bore: across, where the barrel ends
	WeaponShape _vmShape = {};
	// ...and given volume: voxels, each picture pixel as deep as its distance from the outline
	// allows (a cushion), as the faces that show (corners in picture x, y and depth)
	struct VoxelFace { float corner[4][3]; Uint8 color; };
	std::vector<VoxelFace> _vmFaces;
	// dual wield (option): the gun in the other hand, drawn on the other side (its picture, shape
	// and voxels, swapped in while it's drawn), and its flash when its round leaves
	Surface *_offSprite = nullptr;
	const RuleItem *_offRules = nullptr;
	WeaponShape _offShape = {};
	std::vector<VoxelFace> _offFaces;
	MuzzleFlash _offFlash = {};
	Uint32 _offFlashUntil = 0;
	Uint32 _offShotSeen = 0;
	void swapOffHand();
	BattleItem *offHand(BattleUnit *unit, const BattleItem *weapon) const;
	BattleItem *dualPartner(BattleUnit *unit, const BattleItem *weapon, BattleActionType slot, BattleActionType &mode) const;
	void makeViewModel();
	void analyzeWeapon(bool gun);
	void voxelizeWeapon();
	void drawWeaponPlate(bool left, double bob[2], double kick, double dip, int shade, double tip[2]);
	void drawWeaponVoxels(const double nearEnd[3], const double along[3], const double up[3], const double normal[3], double perPixel, int shade);
	void fillQuad(const double corners[4][2], Uint8 color);
	void drawHeldItem(bool left, double bob[2], double dip, int shade);
	Surface *_crosshair;
	Text *_txtMode, *_txtShot, *_txtBanner;
	std::vector<Text*> _bannerGlow;            // the banner again around itself, glowing
	std::unordered_set<Surface*> _own;         // all our surfaces, for blitting without the classic HUD
	enum Anchor { TOP_LEFT, TOP_CENTER, TOP_RIGHT, CENTER, BOTTOM_LEFT, RIGHT_CENTER };
	std::unordered_map<Surface*, Anchor> _anchors; // which screen corner each of ours keeps to on a resize
	bool _wanted;       // the player switched to 3D
	bool _classicHud;   // original bottom HUD drawn over the 3D view
	bool _mouseLook;    // mouse turns the view (captured) instead of moving a cursor
	bool _dragLook;     // right button held over the view in cursor mode
	Uint32 _lastTick;
	int _warningY;
	bool _captured;            // capture state seen last frame
	Uint32 _ignoreMotionUntil; // the cursor's jump to the window centre after grabbing isn't a look
	// camera and movement
	BattleUnit *_trackedUnit, *_lastPlayerUnit;
	Position _trackedTile;  // tile the camera is in (already the destination while a step is being walked)
	double _posX, _posY, _eyeZ;
	double _velX = 0.0, _velY = 0.0;  // walking momentum, voxels per second
	double _bobPhase = 0.0, _bobAmount = 0.0;
	bool _stepPending;
	Position _stepFrom;
	bool _throughDoor;      // the pending step goes through a door: wait for it to open, then walk through
	int _doorStep[2];
	Uint32 _doorStepStart;
	int _stepAllowed[8];    // per frame: -1 unknown, 0 blocked, 1 allowed
	// jumping: a free hop of the camera; into a low obstacle, a vault over it (the step is walked
	// by the rules, the pathfinding letting that obstacle through)
	Uint32 _jumpStart = 0;     // 0: on the ground
	Uint32 _jumpTime = 0;      // how long this jump lasts, ms
	double _jumpHeight = 0.0;  // voxels at the top
	bool _vaulting = false;    // the camera glides over the obstacle
	double _vaultFrom[2] = {}, _vaultTo[2] = {};
	// spotting a new enemy stops the walk, like it interrupts a move on the isometric map
	BattleUnit *_spotter;
	size_t _spottedCount;
	bool _movementHeld;     // until the movement keys are let go
	Uint32 _spottedUntil;   // "enemy spotted" banner
	// getting hurt and going down
	const BattleUnit *_healthUnit;
	int _lastHealth;
	Uint32 _hurtStart;
	FirstPersonSounds *_sounds = nullptr;
	Uint32 _hitMarkHeard = 0;  // the last hitmarker that ticked
	int _heartbeat = -1;       // the last heartbeat that thumped
	float _hurtAmount;
	BattleUnit *_deathUnit; // the soldier we were looking through went down (or was taken over): stay a moment
	Uint32 _deathStart;
	double _deathEye = 0.0;         // the eye height they fell from (a zombie gets up to about it again)
	// on fire: the view in flames, a flare-up and a banner when the soldier catches fire
	const BattleUnit *_burnUnit = nullptr;
	int _lastFire = 0;
	Uint32 _burningUntil = 0;
	enum FallKind { FALL_KILLED, FALL_UNCONSCIOUS, FALL_CONTROLLED, FALL_PANIC, FALL_BERSERK, FALL_ZOMBIE };
	FallKind _fallKind = FALL_KILLED;
	bool _allLost = false;          // that was the last soldier standing
	Text *_txtFallName = nullptr;   // whose camera it was, under the banner
	Uint32 _afterFall = 0;          // the next soldier's view coming in out of the static or the colour
	FallKind _afterFallKind = FALL_KILLED;
	Uint32 _afterFallTime = 0;      // ...over this long
	// a soldier's breakdown at the start of the turn (panic, berserk): the view washes out meanwhile
	BattleUnit *_breakdownUnit = nullptr;
	bool _breakdownBerserk = false;
	Uint32 _breakdownStart = 0;     // when the view started showing it (0: not yet)
	bool _breakdownShot = false;    // a shot was in the air last frame (each new one flashes red)
	Uint32 _breakdownFlash = 0;
	std::unordered_map<const BattleUnit*, std::pair<int, bool>> _brokeDown; // who broke down in which turn, berserk or not (their camera says so)
	bool _musicTense = false;   // the combat track is playing
	// guided missiles, psi attacks and the mind probe can be aimed on the isometric map
	enum MapAimPhase { MAPAIM_NONE, MAPAIM_AIMING, MAPAIM_ACTING };
	MapAimPhase _mapAim = MAPAIM_NONE;
	BattleActionType _mapAimType = BA_NONE;
	Uint32 _lastEnemySeen = 0;
	double _footstepDistance;
	int _footstepPhase;
	// weapons and aiming
	std::unordered_map<const BattleUnit*, int> _hands; // each soldier's chosen hand: 0 right, 1 left
	// FPS-style combat (setting "Faithful X-COM targeting" off): rounds go along the crosshair with
	// a spread, recoil lifts the aim, holding the trigger in auto keeps firing bursts
	bool _triggerHeld = false;
	Uint32 _recoilShotSeen = 0, _lastRecoil = 0;
	double _recoilDebt = 0.0;   // degrees the recoil lifted the view, eased back after firing
	double _bloom = 0.0;        // extra spread from the last rounds (degrees, 95% circle)
	bool freeAimOn(BattleActionType mode) const;
	BattleActionType firedAs(const BattleItem *weapon, BattleActionType mode) const;
	double spreadFor(const BattleUnit *unit, BattleActionType mode, bool dual = false) const;
	void updateRecoil(BattleUnit *unit, double dt);
	BattleActionType _mode;         // fire mode for the left mouse button
	const BattleItem *_modeWeapon;  // the weapon _mode was chosen for
	Position _aimVoxel;             // what's under the crosshair
	bool _aimValid;
	Uint32 _crosshairKey;           // what the crosshair surface shows, to redraw only on change

	Text *addText(int w, int h, int x, int y, const std::string &id, const std::string &category);
	NumberText *addNumber(int x, int y, const std::string &id);
	void syncShadows();
	void setActive(bool active);
	void remember() const;
	void setClassicHud(bool on);
	void setMouseLook(bool on);
	void look(int dx, int dy);
	void turnWithKeys(double dt);
	void faceLookDirection(BattleUnit *unit);
	void lookAtEnemy(int index);
	void updateCamera(BattleUnit *unit, bool canMove, double dt);
	void snapToTile(BattleUnit *unit);
	void move(BattleUnit *unit, bool canMove, double dt);
	void footsteps(BattleUnit *unit, double distance);
	double clampAxis(int tile, double value, int offset) const;
	double eyeHeight(const BattleUnit *unit) const;
	bool stepAllowed(BattleUnit *unit, int dx, int dy);
	bool canCommit() const;
	bool commitStep(BattleUnit *unit, int dx, int dy);
	bool closedDoor(int dx, int dy, int z) const;
	bool walkThroughDoor(BattleUnit *unit, double &nx, double &ny, double dt);
	void jump(BattleUnit *unit);
	bool tryVault(BattleUnit *unit, int dx, int dy);
	bool lowObstacle(const Tile *tile, int part) const;
	bool vaultGlide(BattleUnit *unit, double &nx, double &ny);
	double jumpLift(BattleUnit *unit);
	void endVault();
	bool hasNightVision(const BattleUnit *unit) const;
	void toggleNightVision(BattleUnit *unit, Action *action);
	BattleItem *activeWeapon(BattleUnit *unit) const;
	std::vector<BattleActionType> modesFor(BattleUnit *unit, BattleItem *weapon) const;
	void updateFireMode(BattleUnit *unit);
	void cycleMode(int step);
	void selectMode(BattleActionType mode);
	void switchHand();
	void aim(BattleUnit *unit);
	bool aimAt(int px, int py, BattleUnit *unit, Position &voxel);
	void fire(int px, int py);
	void updateCrosshair(BattleUnit *unit, bool playerTurn);
	void drawCrosshair(BattleActionType mode, int shots, Uint8 color, Uint8 shadow, int hitMark, Uint8 hitColor);
	void applyDoom(BattleUnit *unit);
	void refreshSquad();
	void layoutTopBar();
	void updateTopBar(BattleUnit *selected);
	void updatePanel(BattleUnit *unit);
	void updateArmor(BattleUnit *unit);
	SquadState squadState(const BattleUnit *unit) const;
	void drawSquadIcon(Surface *icon, SquadState state);
	void scrollSquad(int step);
	TextButton *makeButton(int x, int w, const std::string &label);
	// big screens: our overlays are laid out on a canvas of (screen / _ui) and drawn magnified
	int _ui = 1;
	Surface *_canvas = nullptr;
	SDL_Color _canvasPalette[256];
	std::vector<Uint8> _magnifyRow;       // a canvas row widened for the screen
	static int uiScaleFor(int width, int height);
	int canvasWidth() const;
	int canvasHeight() const;
	void makeCanvas();
	void shiftAnchored(int dX, int dY);
	void handleOnCanvas(TextButton *button, Action *action);
	void magnify(SDL_Surface *screen, bool transparent);
	int warningY() const;
	int labelWidth(const std::string &label) const;
	bool canFly(const BattleUnit *unit) const;
	bool flightOn(const BattleUnit *unit) const;
	void toggleFlight(BattleUnit *unit);
	void moveUpDown(BattleUnit *unit, int direction, Action *action);
	void updateFlight(BattleUnit *unit);
	std::string weaponLabel(BattleUnit *unit) const;
	std::string statusLabel(BattleUnit *unit) const;
	void updateCard();
	void openDoor(BattleUnit *unit);
	void updateViewModel(BattleUnit *unit);
	void throwFlare();
	void updateBanner(bool playerTurn);
	void checkSpotted(BattleUnit *unit, bool playerTurn);
	void checkHurt(BattleUnit *unit, Uint32 now);
	void checkBurning(BattleUnit *unit, Uint32 now);
	void updateMusic(Uint32 now);
	void startMapAim(BattleUnit *unit, BattleItem *weapon, BattleActionType mode, const char *hint);
	void updateMapAim();
	bool creatureAt(const Position &voxel, const BattleUnit *self) const;
	bool deathCam(Uint32 now, double dt);
	bool breakdownCam(Uint32 now, double dt);
	void endBreakdown(bool forget = true);
	void showFall(FallKind kind, const std::string &title, const std::string &name);
	TextButton *buttonAt(double x, double y) const;
	void buttonClicked(TextButton *button, Action *action);
	void selectUnit(BattleUnit *unit);
public:
	/// An FPS-style shot's next round, asked for by the game (see FirstPerson::freeAimTarget).
	static void freeAim(BattlescapeState *state, const BattleAction &action, Position &target);
	static bool fastShots(BattlescapeState *state, const BattleUnit *attacker);
	/// A soldier panics or goes berserk, asked by the game (see FirstPerson::breakdown).
	static bool breakdown(BattlescapeState *state, BattleUnit *unit);
	void freeAimTarget(const BattleAction &action, Position &target);

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
	/// Blits the screen without the classic HUD when that's hidden; returns false to let the state blit normally.
	bool blit();
	/// Should the mouse be captured for mouse look right now?
	bool wantsMouseCapture() const;
	/// Puts our surfaces right after the screen was resized, and remakes the 3D view at the new size.
	void resized(int dX, int dY);
};

}
