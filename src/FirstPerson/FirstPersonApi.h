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
 * FirstPersonApi.h: the OpenXcom3D first-person add-on as the rest of the engine sees it. This
 * is the only add-on header the core includes; each function below is called from one small
 * hook in a core file (named with it).
 *
 * Built with -DOXCE_FIRSTPERSON=OFF (which defines OXCE_NO_FIRSTPERSON), every call below is an
 * inline function that does nothing, and the add-on's sources are left out of the build: that's
 * stock OXCE. Built with it, the add-on still only switches on when an active mod declares
 * `firstPerson: enabled: true` (the OpenXcom3D mod does).
 */
#include <map>
#include <string>
#include <vector>
#include "../Menu/OptionsBaseState.h" // OptionsOrigin
#include "../Battlescape/Position.h"

namespace OpenXcom
{

class Action;
struct BattleAction;
class BattleItem;
class BattlescapeState;
class BattleUnit;
class Camera;
class FirstPersonMode;
class Game;
class Mod;
class OptionInfo;
class RuleItem;
class SavedBattleGame;
class State;
class Text;
class Surface;
class Tile;

namespace FirstPerson
{

/// Akimbo (dual wield): the share of its accuracy each gun keeps when both fire at once, a 20%
/// penalty (a player's report on 1.03: it was half, then 30%; a player, GitHub issue #87: it
/// stacks with the game's two-handed penalty, the owner cut it to 20%). The game's shot and its
/// pellets and the other gun's round use it (ProjectileFlyBState), and the 3D view's spread and
/// hit chance.
const double DualWieldAccuracy = 0.8;
/// Akimbo: two guns fired at once cost the dearer gun's shot and this share more (a player's report:
/// two identical guns cost twice one shot, at 30% less accuracy each; the owner's choice). The chosen
/// gun pays its own shot as the game charges it, the other gun the rest (dualWieldCharge). Guns that
/// fire in turn (an arcing one) are two shots at full accuracy, and pay in full.
const double DualWieldExtraCost = 0.1;
/// What the other gun pays when both fire, for one kind of cost (time units, energy): the pair's
/// cost (the dearer gun's and DualWieldExtraCost more, rounded) less what the chosen gun pays.
inline int dualWieldCharge(int chosen, int other)
{
	const int pair = (int)((chosen > other ? chosen : other) * (1.0 + DualWieldExtraCost) + 0.5);
	return pair > chosen ? pair - chosen : 0;
}

/// Setting "Auto-equip guns first": the auto-equip's last pass (the rest of the kit), see autoEquipTakes.
const int AutoEquipLeftoversPass = 5;

#ifndef OXCE_NO_FIRSTPERSON

// ---- A 3D view at the display's own size (FirstPersonPresent.cpp; hooks in Screen::flip and Zoom::flipWithZoom) ----

/// Setting "3D resolution": the 3D view the size of its HUD's canvas (magnified with it), of the battlescape, or of the display.
enum ViewResolution { ViewResolutionHud, ViewResolutionBattlescape, ViewResolutionDisplay };
/// Whether a view the display's own size can be shown.
bool canPresentAtDisplaySize();
/// The value to fill the game's screen with where a view the display's size is to show through (see presentView).
Uint32 presentKey(SDL_Surface *screen);
/// Asks for a view (8-bit, the size of the display's picture area) to be laid into the display at this frame's flip, where the screen still holds presentKey.
void presentView(const Surface *view, const Uint32 *fullColors = nullptr);
/// Screen::flip's hook: lays the view asked for into the display.
void presentAtFlip(SDL_Surface *display, int x, int y, int width, int height);
/// Zoom::flipWithZoom's hook, with the engine's OpenGL output: draws the view asked for over the window.
void presentAtFlipOpenGl(SDL_Surface *screen, int x, int y, int width, int height, int displayHeight);


// ---- The battlescape screen (BattlescapeState.cpp) ----

/// Creates the first-person mode for a battlescape screen, or returns nullptr unless an active
/// mod enables it. Called when the screen is made.
FirstPersonMode *create(Game *game, BattlescapeState *state);
/// Destroys it. Called when the screen goes away.
void destroy(FirstPersonMode *mode);
/// The per-frame update: camera, movement, HUD. Called from the screen's think().
void think(FirstPersonMode *mode);
/// Gets the first look at an event (keys, mouse); returns true if it used it, and the screen
/// then leaves it alone. Called from the screen's handle().
bool handle(FirstPersonMode *mode, Action *action);
/// Draws the battlescape screen its own way (our overlays are magnified from a canvas, and the
/// classic HUD may be hidden); returns false when there is no first-person mode, and the screen
/// then blits as usual. Called from the screen's blit().
bool blit(FirstPersonMode *mode);
/// Should the mouse be captured for mouse look right now? Asked by the screen each frame.
bool wantsMouseCapture(const FirstPersonMode *mode);
/// The screen was resized by (dX, dY) and moved every surface like its own bottom-centred HUD:
/// puts ours where they belong. Called from the screen's resize().
void resized(FirstPersonMode *mode, int widthChange, int heightChange);

// ---- Options (Options.cpp, OptionsBaseState.cpp) ----

/// Registers the add-on's options (the fps* settings and their keys). Called while the game
/// builds its list of options.
void createOptions(std::vector<OptionInfo> &options);
/// Should the options screen offer the 3D VIEW tab (only when an active mod enables the mode)?
bool hasOptionsTab(const Mod *mod);
/// Creates the 3D VIEW options tab. Called when the player clicks it.
State *createOptionsTab(OptionsOrigin origin);
/// Settles settings that take their first value from an older one (the smoke from "See-through
/// smoke", the flashlight from 1.0's two flashlight settings, the next fire mode key from 1.0's).
void settleOptions();
/// Setting "Playstyle": chooses Faithful (0), FPS (1) or Custom (2), setting its switches.
void choosePlaystyle(int playstyle);
/// Setting "Playstyle": notes one of its switches changed by hand (the playstyle the switches make now, Custom keeping them).
void playstyleSwitchChanged();
/// Setting "Playstyle": whether a setting is one of the switches it sets.
bool isPlaystyleSwitch(const std::string &id);
/// Setting "Playstyle": settles it with the switches as they are (a profile from before it: Custom unless they match).
void settlePlaystyle();
/// Does a mod the game finds for the first time (one not yet in the player's mod list) start
/// switched on? Our own mod does, so a game set up by hand plays in 3D at once; switched off, it
/// stays off.
bool activeWhenNew(const std::string &modId);
/// The smoke setting as it stands: 1 Original, 2 Modern (unset, or 1.0's Solid: see the function).
int smokeMode();
/// The flashlight setting as it stands: 0 Off, 1 On, 2 Dynamic (unset: from 1.0's settings).
int flashlightMode();
/// The akimbo setting as it stands: 0 Off, 1 Full, 2 Realistic (unset: from 1.08's two settings).
int akimboChoice();
/// The longest Doom grenade fuse, in seconds (setting "Doom grenade fuse": 1 to this).
const int DoomFuseMaxSeconds = 5;
/// Setting "View distance": its first value (the reach the mod sets), and the steps it goes round, in tiles.
const int DefaultViewDistance = 60;
const int ViewDistanceSteps[] = { 60, 80, 100, 130, 160, 200 };
/// How far the big 3D view reaches, in tiles (setting "View distance"): the mod's reach
/// (maxViewDistance) at the first value, else the distance chosen.
int viewDistance(const Mod *mod);

// ---- The battle (ProjectileFlyBState.cpp, ExplosionBState.cpp, BattlescapeGame.cpp) ----

/// An FPS-style shot's next round ("Faithful X-COM targeting" off): where the first-person view
/// aims it now (along the crosshair, with a spread and the recoil so far). The game's own
/// projectile code asks this for each round, and then traces the round with its usual line of
/// fire.
void freeAimTarget(BattlescapeState *state, const BattleAction &action, Position &target);
/// An FPS-style straight shot's origin: it leaves from where the first-person view's eye is
/// across the soldier's tile (a lean too), at the game's gun height, so what the crosshair sees
/// past a corner the round can reach. The game asks this as it works out where a shot leaves
/// from (TileEngine::getOriginVoxel); every other shot keeps the game's own muzzle point.
void freeAimOrigin(BattlescapeState *state, const BattleAction &action, Position &origin);
/// Where an arcing shot or a throw from the first-person view leaves from (setting "Arcs: smooth
/// start"): a point that slides round the unit's footprint with the exact bearing to the target,
/// between the game's eight (given as their shifts across a tile, by direction). Asked by the
/// game after it picked its own point (TileEngine::getOriginVoxel).
void slidingOrigin(BattlescapeState *state, const BattleAction &action, const Position &from, const int shiftX[8], const int shiftY[8], Position &origin);
/// Is FPS-style shooting on for this unit's shots in the first-person view? Their bullet hits
/// then play quicker (a burst at a bullet's pace).
bool fastShots(BattlescapeState *state, const BattleUnit *attacker);
/// Setting "Full sprite hitboxes": is a unit hit where its picture shows, in a line of fire (the
/// battle's rule, in 3D and on the map, whoever fires), rather than by the game's voxel shapes?
bool spriteHitboxes(BattlescapeState *state);
/// Setting "Full sprite hitboxes": which unit's picture this voxel of a line of fire hits (the line
/// from origin to target, the serial number TileEngine gave it; the unit left out, units unseen
/// passed through, or only one tested, as the game asks): where the line crosses the picture's
/// card, as the 3D view draws it, on a solid pixel; any unit's that reaches the line, not only the
/// one on the voxel's tile (see FirstPersonSpriteHits.cpp). Null for none.
BattleUnit *spriteHitAt(SavedBattleGame *save, const Position &voxel, const Position &origin, const Position &target, unsigned serial,
	const BattleUnit *excludeUnit, bool onlyVisible, const BattleUnit *excludeAllBut);
/// Setting "Full sprite hitboxes": where the game counts a round that met a unit's picture as
/// stopping: the voxel, or the nearest of the unit's own tiles' (a picture reaching over a
/// neighbouring tile, or above its level, was hit there; the hit is worked out on the unit's tile).
Position spriteImpact(SavedBattleGame *save, const BattleUnit *unit, const Position &voxel);
/// Setting "Full sprite hitboxes": where a round that met a unit's picture struck it, relative to
/// its middle, for the armour side and body part hit: pushed out towards the shooter, as the
/// game's own hit shape would have stopped it on that side.
Position spriteHitRelative(const Position &relative, const BattleUnit *unit, const BattleUnit *shooter);
/// Setting "Full sprite hitboxes": the curve of a throw or an arcing shot is walked as many short
/// lines, a step of the curve each (TileEngine::calculateParabolaVoxel); while one of these lives,
/// the pictures those lines meet face the curve's start (the hand it left), as a straight line's
/// face its start, each step still tested where it crosses the picture. Each step's own start (the
/// round itself) turned the picture to face it, and a curve met a picture only within a voxel of
/// the unit's middle (a player, GitHub issue #119, XPiratez's bows: "you can CLEARLY see the arrow
/// hit it but the game acts like I hit the ground next to it"). With the setting off, nothing.
class ArcLines
{
public:
	ArcLines(SavedBattleGame *save, const Position &origin);
	~ArcLines();
	ArcLines(const ArcLines&) = delete;
	ArcLines &operator=(const ArcLines&) = delete;
private:
	bool _set;              // the setting was on: the curve's start set for its lines
	bool _outerActive;      // the curve's start as it was before (none: no curve is walked inside another)
	double _outerFrom[3];
};
/// Setting "Full sprite hitboxes": where an arcing shot at a unit aims across the map (the game aims
/// at the tile's middle, Projectile::calculateThrow): a big unit at its own middle, where its tiles
/// meet and its picture stands; any other left as it is. With the setting off, left as it is.
void arcAimAtPicture(BattlescapeState *state, const BattleUnit *unit, Position &aim);
/// Setting "Full sprite hitboxes": does a curve the game tries for an arcing shot land on what it's
/// aimed at by meeting the unit on the aimed tile, on any of that unit's tiles (TileEngine::
/// validateThrow takes only a curve that comes down on the aimed tile, and a big unit's picture,
/// met in its middle, often stops it over another of its tiles: the curve went higher and higher
/// until it came down on the floor beside the unit, GitHub issue #119)? Never for a throw (an item
/// stopped by a unit drops short of its tile), nor with the setting off.
bool arcMeetsAimedUnit(SavedBattleGame *save, bool thrown, int meets, const Position &impact, const Position &aimTile);
/// In a battle with the mod's 3D view, a unit turning to a point on its own tile (straight above
/// or below it) keeps its facing, where the game turned it south (BattleUnit::directionTo).
bool keepsFacingOnOwnTile();
/// Setting "Auto-equip guns first": does the game's auto-equip hand out guns before melee weapons,
/// the strongest first (BattlescapeGenerator::autoEquip), rather than its own way?
bool autoEquipGunsFirst(const Mod *mod);
/// The main menu: the mod's version in small letters just right of the title's "OpenXcom3D" (none without the mod).
void addMenuVersion(State *state, Text *title, const Mod *mod);
/// Setting "Auto-equip guns first": the items offered, the hardest-hitting first (then the mod's list order, then by type).
void sortAutoEquip(std::vector<BattleItem*> &items);
/// Setting "Auto-equip guns first": does this pass of the auto-equip take this item? Passes 0 to
/// AutoEquipLeftoversPass: two-handed guns, one-handed guns, two-handed melee weapons, one-handed
/// ones, ammo, the rest (flares only when it's dark enough, takeFlares).
bool autoEquipTakes(const RuleItem *rule, int pass, bool takeFlares);
/// Setting "Quick deaths": is a unit killed or knocked out down at once for the battle, its fall
/// playing on its own (in 3D and on the map, several at once; see FirstPersonQuickDeaths.cpp)?
bool quickDeaths(BattlescapeState *state);
/// Setting "Quick deaths": notes a unit going down quickly, as the game takes it out (its fall
/// plays from now on).
void noteQuickFall(const SavedBattleGame *save, BattleUnit *unit);
/// Setting "Quick deaths": forgets the falls under way (a battle starting: its units are new).
void clearQuickFalls();
/// Setting "Quick deaths": the frame of a unit's quick fall to draw, or -1 if it isn't falling.
int quickFallPhase(const BattleUnit *unit);
/// Setting "Quick deaths": the unit whose quick fall plays on this tile (drawn there falling, the
/// tile's items held back meanwhile), or null.
BattleUnit *quickFallOn(const SavedBattleGame *save, const Tile *tile);
/// Setting "Quick deaths": while it lives, a unit in its quick fall is in that fall's pose for
/// drawing (the game draws a falling unit by its status and fall frame); a unit that isn't
/// falling, or null, is left alone.
class QuickFallPose
{
public:
	explicit QuickFallPose(BattleUnit *unit);
	/// The same, at a frame of the fall given (a body coming down from the air, see airFallHeight).
	QuickFallPose(BattleUnit *unit, int phase);
	~QuickFallPose();
	QuickFallPose(const QuickFallPose&) = delete;
	QuickFallPose &operator=(const QuickFallPose&) = delete;
private:
	BattleUnit *_unit; // the unit posed (null: none)
};
/// A unit going down as it hovers over no floor (GitHub issue #62): notes it as the game takes it
/// out, so the 3D view draws it falling to the ground, the game itself having it there at once.
void noteAirFall(const SavedBattleGame *save, BattleUnit *unit);
/// Forgets the falls from the air under way (a battle starting: its units are new).
void clearAirFalls();
/// Where a unit coming down from the air is now: its tile across (x, y) and its feet's height in
/// voxels; false if it isn't falling (never was, or it has landed).
bool airFallHeight(const BattleUnit *unit, Position &tile, double &feetZ);
/// Whether a body coming down from the air lands on this tile and hasn't yet (its items held back).
bool airFallLandsOn(const SavedBattleGame *save, const Tile *tile);
/// Setting "Shotgun pellets in flight": do a shotgun's pellets fly beside its round (in 3D and on
/// the map), rather than all landing at once? They're traced as the round is fired, and hit as it lands.
bool pelletsInFlight(BattlescapeState *state);
/// Setting "Instant chain explosions": do the explosives a blast sets off go up at once, the whole
/// chain one event under that blast, the 3D view showing it as one big blast (in 3D only)?
bool instantChains(BattlescapeState *state);
/// Setting "Instant chain explosions" (a player, GitHub issue #105: "1 big boom"): an explosive of a
/// chain settling at once goes up, noted for the 3D view's ripple of fireballs before its blast
/// changes the map (the chain's first keeps the map and the units as they were, see
/// FirstPersonChains.cpp).
void chainLink(SavedBattleGame *save, const Position &centre, int power, bool first);
/// Setting "Instant chain explosions": the chain's last explosive has settled (none other set off):
/// its picture is worked out, before the 3D view's next frame (see FirstPersonChains.cpp).
void chainSettled(SavedBattleGame *save);
/// Setting "Instant chain explosions": does the battle wait while the 3D view shows a chain settled
/// at once rippling through (its dead fall after it)?
bool chainRippleHolds(BattlescapeState *state);
/// Setting "Each pellet on its own": do a shotgun's pellets and a volley's two rounds each land on
/// their own (in either view), none lost when the lead pellet leaves the map?
bool eachPellet(BattlescapeState *state);
/// The stat bars (time units, energy, health, morale, mana: StatBar), setting "Stat bars": the scale,
/// in pixels per point, a bar of this width is drawn at for a unit's maximum of the stat (the map's; the 3D view's alike):
/// one, or fitted (one scale per stat for all the player's units, so the longest maximum fits and
/// different maxima stay different lengths), or the bar full at the unit's own maximum.
enum StatBar { STAT_TU, STAT_ENERGY, STAT_HEALTH, STAT_MORALE, STAT_MANA };
double statBarScale(BattlescapeState *state, int stat, int maximum, int barWidth);
/// Akimbo on the map (Ctrl and Shift held, a click on a hand's weapon): steps the soldier's two
/// guns through firing together on the map, both at once, in turn, and not. True if it took the click.
bool toggleMapAkimbo(BattlescapeState *state, BattleUnit *unit);
/// Akimbo on the map: its mini targets in both hands' boxes (one each both at once, two in turn),
/// drawn after the game draws the hands; undone once the two guns no longer pair up.
void drawMapAkimbo(BattlescapeState *state, BattleUnit *unit, Surface *leftHand, Surface *rightHand);
/// A shot fired on the map, as it's launched: with akimbo marked there, the other hand's gun
/// fires along, as in 3D (not while the 3D view shows: its shots bring their own).
void mapAkimbo(BattlescapeState *state, BattleAction &action);
/// A reaction shot, as it's launched, in either view: with the soldier's akimbo marked on the map,
/// the other hand's gun fires along.
void reactionAkimbo(BattlescapeState *state, BattleAction &action);
/// Reaction fire came at this unit (TileEngine::checkReactionFire: a reactor shot at it or struck
/// it), in either view: as the game stops a walk on the map then, the 3D view stops the walk of the
/// soldier it looks through (the movement keys are ignored until they're all let go).
void reactionFiredAt(BattlescapeState *state, const BattleUnit *unit);
/// A round or a blow struck a unit (TileEngine::hit, after the damage): an enemy in sight it did
/// no harm marks the 3D view's crosshair (its health and stun before the strike given).
void unitStruck(BattlescapeState *state, const BattleUnit *unit, int healthBefore, int stunBefore);
/// Setting "Damage numbers": a shot, a blow or a blast hurt a unit, in either view (TileEngine::hitUnit,
/// the one place each of them deals its damage, after it): the health it lost, the armour it took off
/// (the side struck) and the stun it took, to rise over it in 3D and on the map if the squad sees it
/// (see FirstPersonDamageNumbers.cpp).
void unitDamaged(BattlescapeState *state, const BattleUnit *unit, int healthLost, int armorLost, int stunTaken);
/// Setting "Damage numbers": a unit's armour, all its sides' points together (TileEngine::hitUnit reads
/// it before and after a hit: what the hit took off the side it struck).
int armorPoints(const BattleUnit *unit);
/// Setting "Damage numbers": draws the numbers rising over the units hit on the isometric map, over
/// all of it (Map::drawTerrain, last).
void drawMapDamageNumbers(BattlescapeState *state, Surface *surface, const Camera *camera);
/// A shotgun's pellet landed (ProjectileFlyBState::pelletLands), in either view: it's heard (a few
/// to a volley; not one whose hit the explosion state brings, which sounds its own) and the 3D view
/// marks where it struck (sparks, a bullet hole), as it does the round.
void pelletLanded(BattlescapeState *state, const Position &at, const Position &from, int impact, const BattleItem *weapon, const BattleItem *ammo, bool heard = true);
/// A round (or a thrown item) took a step along its path (Projectile::move: `voxel` where it is now,
/// `from` a few steps back), in either view: glass the 3D view fills into a window's opening there
/// (the game's rounds go through the opening) shatters, for the rest of the battle; its shards and
/// sound only while the 3D view shows. Only the look: the game's rules never know.
void roundPassed(BattlescapeState *state, const Position &voxel, const Position &from);
/// A blast that harms terrain reached these tiles (TileEngine::explode, the damage it does each), in
/// either view: the glass in the openings of their windows shatters, as roundPassed's.
void blastReached(BattlescapeState *state, const Position &center, const std::map<Tile*, int> &tiles);
/// The bullet sprite for shotgun ammo that has none (Projectile): the one the mod's other ammo of
/// its damage type uses most, so the map can draw its round and pellets in flight.
int shotgunBulletSprite(const Mod *mod, const RuleItem *ammo);
/// A unit panics or goes berserk: returns true if the first-person view shows it its own way
/// (the game then leaves out its "has panicked" box, which would freeze the view behind it).
bool breakdown(BattlescapeState *state, BattleUnit *unit);
/// A close-quarters check failed: the shooter's shot is knocked aside at this tile, and the game
/// has turned the shooter to face it. The first-person view looks there too, so the soldier
/// isn't turned back to the old view afterwards (at a cost, with "Turning costs TU").
void closeQuartersDeflected(BattlescapeState *state, const BattleUnit *shooter, const Position &target);

// ---- Items and units (BattleItem.cpp, BattleUnit.cpp) ----

/// Doom IDKFA: does a shot by this unit spend no ammo (the selected soldier, on the player's
/// turn, in the 3D view)? Asked whenever a weapon would spend ammo, beside the game's own debug
/// mode, which spends none either.
bool endlessAmmo(BattlescapeState *state, const BattleUnit *unit);
/// Doom IDDQD: does this unit take no damage at all (X-COM's units)? Asked whenever a unit is
/// about to be hurt.
bool invincible(BattlescapeState *state, const BattleUnit *unit);
/// Doom IDCLIP: does this unit float where it is rather than fall (any of X-COM's units, either view)?
bool noclipFloats(BattlescapeState *state, const BattleUnit *unit);

#else

// Stock OXCE: the add-on is left out, and every hook does nothing.
inline void presentAtFlip(SDL_Surface *, int, int, int, int) { }
inline void presentAtFlipOpenGl(SDL_Surface *, int, int, int, int, int) { }
inline FirstPersonMode *create(Game *, BattlescapeState *) { return nullptr; }
inline void destroy(FirstPersonMode *) { }
inline void think(FirstPersonMode *) { }
inline bool handle(FirstPersonMode *, Action *) { return false; }
inline bool blit(FirstPersonMode *) { return false; }
inline bool wantsMouseCapture(const FirstPersonMode *) { return false; }
inline void resized(FirstPersonMode *, int, int) { }
inline void createOptions(std::vector<OptionInfo> &) { }
inline bool hasOptionsTab(const Mod *) { return false; }
inline State *createOptionsTab(OptionsOrigin) { return nullptr; }
inline bool activeWhenNew(const std::string &) { return false; }
inline void freeAimTarget(BattlescapeState *, const BattleAction &, Position &) { }
inline void freeAimOrigin(BattlescapeState *, const BattleAction &, Position &) { }
inline void slidingOrigin(BattlescapeState *, const BattleAction &, const Position &, const int[8], const int[8], Position &) { }
inline bool fastShots(BattlescapeState *, const BattleUnit *) { return false; }
inline bool pelletsInFlight(BattlescapeState *) { return false; }
inline bool instantChains(BattlescapeState *) { return false; }
inline void chainLink(SavedBattleGame *, const Position &, int, bool) { }
inline void chainSettled(SavedBattleGame *) { }
inline bool chainRippleHolds(BattlescapeState *) { return false; }
inline bool eachPellet(BattlescapeState *) { return false; }
enum StatBar { STAT_TU, STAT_ENERGY, STAT_HEALTH, STAT_MORALE, STAT_MANA };
inline double statBarScale(BattlescapeState *, int, int, int) { return 1.0; }
inline bool toggleMapAkimbo(BattlescapeState *, BattleUnit *) { return false; }
inline void drawMapAkimbo(BattlescapeState *, BattleUnit *, Surface *, Surface *) { }
inline void mapAkimbo(BattlescapeState *, BattleAction &) { }
inline void reactionAkimbo(BattlescapeState *, BattleAction &) { }
inline void reactionFiredAt(BattlescapeState *, const BattleUnit *) { }
inline void unitStruck(BattlescapeState *, const BattleUnit *, int, int) { }
inline void unitDamaged(BattlescapeState *, const BattleUnit *, int, int, int) { }
inline int armorPoints(const BattleUnit *) { return 0; }
inline void drawMapDamageNumbers(BattlescapeState *, Surface *, const Camera *) { }
inline void pelletLanded(BattlescapeState *, const Position &, const Position &, int, const BattleItem *, const BattleItem *, bool = true) { }
inline void roundPassed(BattlescapeState *, const Position &, const Position &) { }
inline void blastReached(BattlescapeState *, const Position &, const std::map<Tile*, int> &) { }
inline int shotgunBulletSprite(const Mod *, const RuleItem *) { return -1; } // (Mod::NO_SURFACE)
inline bool breakdown(BattlescapeState *, BattleUnit *) { return false; }
inline void closeQuartersDeflected(BattlescapeState *, const BattleUnit *, const Position &) { }
inline bool endlessAmmo(BattlescapeState *, const BattleUnit *) { return false; }
inline bool invincible(BattlescapeState *, const BattleUnit *) { return false; }
inline bool noclipFloats(BattlescapeState *, const BattleUnit *) { return false; }
inline bool spriteHitboxes(BattlescapeState *) { return false; }
inline BattleUnit *spriteHitAt(SavedBattleGame *, const Position &, const Position &, const Position &, unsigned, const BattleUnit *, bool, const BattleUnit *) { return nullptr; }
inline Position spriteImpact(SavedBattleGame *, const BattleUnit *, const Position &voxel) { return voxel; }
inline Position spriteHitRelative(const Position &relative, const BattleUnit *, const BattleUnit *) { return relative; }
class ArcLines
{
public:
	ArcLines(SavedBattleGame *, const Position &) { }
};
inline void arcAimAtPicture(BattlescapeState *, const BattleUnit *, Position &) { }
inline bool arcMeetsAimedUnit(SavedBattleGame *, bool, int, const Position &, const Position &) { return false; }
inline bool keepsFacingOnOwnTile() { return false; }
inline bool autoEquipGunsFirst(const Mod *) { return false; }
inline void addMenuVersion(State *, Text *, const Mod *) { }
inline void sortAutoEquip(std::vector<BattleItem*> &) { }
inline bool autoEquipTakes(const RuleItem *, int, bool) { return false; }
inline bool quickDeaths(BattlescapeState *) { return false; }
inline void noteQuickFall(const SavedBattleGame *, BattleUnit *) { }
inline void clearQuickFalls() { }
inline int quickFallPhase(const BattleUnit *) { return -1; }
inline BattleUnit *quickFallOn(const SavedBattleGame *, const Tile *) { return nullptr; }
class QuickFallPose
{
public:
	explicit QuickFallPose(BattleUnit *) { }
	QuickFallPose(BattleUnit *, int) { }
};
inline void noteAirFall(const SavedBattleGame *, BattleUnit *) { }
inline void clearAirFalls() { }
inline bool airFallHeight(const BattleUnit *, Position &, double &) { return false; }
inline bool airFallLandsOn(const SavedBattleGame *, const Tile *) { return false; }

#endif

}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
