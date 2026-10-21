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
 * FirstPersonModeWeaponInHand.cpp: the weapon drawn in the soldier's hands.
 *
 * Made from the game's own inventory picture, measured and given volume as voxels, and drawn at
 * the bottom of the view with its muzzle flash, the dip when it changes, recoil, bob and melee
 * swings. With dual wield a second weapon is drawn on the other side.
 */
#include "FirstPersonMode.h"
#include <algorithm>
#include <cmath>
#include "FirstPersonView.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleDamageType.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "FirstPersonModeShared.h"

namespace OpenXcom
{

/**
 * Dual wield: swaps the other hand's gun (its picture, measured shape and voxel faces) in
 * for drawing, and back again on the second call. The drawing code only ever draws the
 * "held" weapon, so the second gun is drawn by swapping it in for a moment.
 */
void FirstPersonMode::swapOffHand()
{
	std::swap(_heldSprite, _offHandSprite);
	std::swap(_heldRules, _offHandRules);
	std::swap(_heldShape, _offHandShape);
	std::swap(_heldFaces, _offHandFaces);
}

/**
 * Works out where a melee swing is (it lasts SwingTime ms): winding up over the first quarter
 * (windup rises 0 to 1), then the strike over the second quarter (windup falls back while
 * strike rises 0 to 1), then pulling back over the second half (strike falls to 0). Each part
 * eases along a quarter of a sine wave.
 * @param windup Set to how far the weapon is drawn back, 0..1.
 * @param strike Set to how far it's thrust in towards the crosshair, 0..1.
 * @return False when there's no swing on (both are then 0).
 */
bool FirstPersonMode::swingPhase(double &windup, double &strike) const
{
	windup = strike = 0.0;
	if (!_swingStart)
	{
		return false;
	}

	// How far through the swing we are, 0..1; past the end, the swing is over.
	const double progress = (SDL_GetTicks() - _swingStart) / (double)SwingTime;
	if (progress >= 1.0)
	{
		return false;
	}

	// Wind up, strike, pull back.
	if (progress < 0.25) windup = std::sin(Pi / 2.0 * progress / 0.25);
	else if (progress < 0.5) { windup = std::cos(Pi / 2.0 * (progress - 0.25) / 0.25); strike = std::sin(Pi / 2.0 * (progress - 0.25) / 0.25); }
	else strike = std::cos(Pi / 2.0 * (progress - 0.5) / 0.5);
	return true;
}

/**
 * Makes the surface the weapon in hand is drawn on: as big as the whole canvas (made again
 * when the canvas changes size). It goes right in front of the 3D view and behind all the rest
 * of the screen, so a weapon in the left hand never covers the info panel.
 */
void FirstPersonMode::makeWeaponInHand()
{
	Surface *surface = new Surface(canvasWidth(), canvasHeight(), 0, 0);
	surface->setPalette(_state->getPalette());
	surface->setVisible(false);
	if (_weaponInHand)
	{
		// Made again (a resize): the new surface takes the old one's place in the screen's list.
		std::replace(_state->_surfaces.begin(), _state->_surfaces.end(), _weaponInHand, surface);
		_ownSurfaces.erase(_weaponInHand);
		delete _weaponInHand;
	}
	else
	{
		// The first time: straight after the 3D view in the screen's drawing order.
		auto &surfaces = _state->_surfaces;
		surfaces.insert(std::find(surfaces.begin(), surfaces.end(), (Surface*)_view) + 1, surface);
	}
	_ownSurfaces.insert(surface);
	_weaponInHand = surface;
}

/**
 * Measures the weapon's inventory picture (the game's own, from BIGOBS.PCK, already drawn into
 * _heldSprite), so it can be drawn pointing at the crosshair whatever way it lies in the
 * picture: its long axis (the direction its pixels spread out furthest), the muzzle at the
 * thinner end (barrels are thin, stocks and grips aren't), and the side with the grip and
 * magazine (the one sticking out further from the axis) down. Then gives it volume
 * (voxelizeWeapon). All measures are in picture pixels, along and across the long axis from
 * the picture's centre of mass.
 * A melee weapon's striking end is the one higher in the picture instead: the games picture
 * them standing up, the business end at the top (a stun rod's prongs, a lance's point, a
 * blade), and that end is often the thicker one.
 * @param gun It's a gun or a club, drawn pointing at the crosshair (false: held up like an item).
 * @param melee It's a melee weapon: held tip up, and swung down at the crosshair (see drawWeaponPlate).
 */
void FirstPersonMode::analyzeWeapon(bool gun, bool melee)
{
	WeaponShape &shape = _heldShape;
	shape = WeaponShape();
	shape.gun = gun;
	shape.melee = melee && gun;
	_heldFaces.clear(); // a picture too small to measure gets none

	// Every opaque pixel of the picture (colour 0 is transparent); too few to measure, and
	// it's held up like an item instead.
	std::vector<std::pair<int, int>> pixels;
	for (int y = 0; y < _heldSprite->getHeight(); ++y)
		for (int x = 0; x < _heldSprite->getWidth(); ++x)
			if (_heldSprite->getPixel(x, y)) pixels.push_back({ x, y });
	if (pixels.size() < 8)
	{
		shape.gun = false;
		return;
	}

	// The centre of mass of the pixels, and the box they fill.
	double meanX = 0, meanY = 0;
	shape.boxMin[0] = shape.boxMin[1] = 1e9;
	shape.boxMax[0] = shape.boxMax[1] = -1e9;
	for (const auto &pixel : pixels)
	{
		meanX += pixel.first; meanY += pixel.second;
		shape.boxMin[0] = std::min(shape.boxMin[0], (double)pixel.first); shape.boxMax[0] = std::max(shape.boxMax[0], (double)pixel.first);
		shape.boxMin[1] = std::min(shape.boxMin[1], (double)pixel.second); shape.boxMax[1] = std::max(shape.boxMax[1], (double)pixel.second);
	}
	meanX /= pixels.size(); meanY /= pixels.size();

	// The long axis: the main direction of the pixels' spread around the centre (from their
	// covariance; dx, dy are a pixel's offsets from the centre).
	double covarianceXX = 0, covarianceYY = 0, covarianceXY = 0;
	for (const auto &pixel : pixels)
	{
		const double dx = pixel.first - meanX, dy = pixel.second - meanY;
		covarianceXX += dx * dx; covarianceYY += dy * dy; covarianceXY += dx * dy;
	}
	const double axisAngle = 0.5 * std::atan2(2.0 * covarianceXY, covarianceXX - covarianceYY);
	shape.mean[0] = meanX; shape.mean[1] = meanY;
	shape.along[0] = std::cos(axisAngle); shape.along[1] = std::sin(axisAngle);
	shape.across[0] = -std::sin(axisAngle); shape.across[1] = std::cos(axisAngle);

	// How far the picture reaches along the axis and across it, both ways from the centre.
	double minAlong = 1e9, maxAlong = -1e9, minAcross = 1e9, maxAcross = -1e9;
	for (const auto &pixel : pixels)
	{
		const double dx = pixel.first - meanX, dy = pixel.second - meanY;
		const double posAlong = dx * shape.along[0] + dy * shape.along[1], posAcross = dx * shape.across[0] + dy * shape.across[1];
		minAlong = std::min(minAlong, posAlong); maxAlong = std::max(maxAlong, posAlong);
		minAcross = std::min(minAcross, posAcross); maxAcross = std::max(maxAcross, posAcross);
	}

	// How thick each end is (across, over the quarter of the length at that end).
	double lowMin = 1e9, lowMax = -1e9, highMin = 1e9, highMax = -1e9;
	const double quarter = (maxAlong - minAlong) * 0.25;
	for (const auto &pixel : pixels)
	{
		const double dx = pixel.first - meanX, dy = pixel.second - meanY;
		const double posAlong = dx * shape.along[0] + dy * shape.along[1], posAcross = dx * shape.across[0] + dy * shape.across[1];
		if (posAlong < minAlong + quarter) { lowMin = std::min(lowMin, posAcross); lowMax = std::max(lowMax, posAcross); }
		if (posAlong > maxAlong - quarter) { highMin = std::min(highMin, posAcross); highMax = std::max(highMax, posAcross); }
	}

	// The muzzle is at the end that's thinner by more than a pixel. With both ends about as
	// thick, it's the end nearer the top of the picture (or the left, for a weapon lying flat).
	// A melee weapon standing up in its picture strikes with its top end.
	const double lowSpread = lowMax - lowMin, highSpread = highMax - highMin;
	const bool standingUp = std::abs(shape.along[1]) > std::abs(shape.along[0]);
	bool muzzleHigh;
	if (shape.melee && standingUp) muzzleHigh = shape.along[1] < 0;
	else if (highSpread < lowSpread - 1.0) muzzleHigh = true;
	else if (lowSpread < highSpread - 1.0) muzzleHigh = false;
	else muzzleHigh = standingUp ? shape.along[1] < 0 : shape.along[0] < 0;
	shape.stock = muzzleHigh ? minAlong : maxAlong;
	shape.muzzle = muzzleHigh ? maxAlong : minAlong;
	shape.minAcross = minAcross;
	shape.maxAcross = maxAcross;

	// "Up" is away from the side that sticks out further (the grip and magazine hang down).
	shape.upSign = maxAcross > -minAcross ? -1 : 1;

	// The bore: the middle (across) of the last two pixels' worth at the muzzle end. That's the
	// point put on the ray to the crosshair (the shot leaves from there), not the middle of
	// the whole weapon.
	double boreSum = 0.0;
	int boreCount = 0;
	for (const auto &pixel : pixels)
	{
		const double dx = pixel.first - meanX, dy = pixel.second - meanY;
		const double posAlong = dx * shape.along[0] + dy * shape.along[1], posAcross = dx * shape.across[0] + dy * shape.across[1];
		if (std::abs(posAlong - shape.muzzle) <= 2.0) { boreSum += posAcross; ++boreCount; }
	}
	shape.bore = boreCount ? boreSum / boreCount : 0.0;
	voxelizeWeapon();
}

/**
 * Gives the held picture volume (the weapon in hand, or a grenade, a medi-kit, a clip...): its
 * voxel model, see voxelizePicture.
 */
void FirstPersonMode::voxelizeWeapon()
{
	voxelizePicture(_heldSprite, _heldFaces);
}

/**
 * Works out how the muzzle flashes, by the damage type of what the weapon fires: a yellow star
 * for bullets, a big orange ball for explosives, red-orange for fire, thin red spikes for lasers
 * (cyan under the sea), a round green glow for plasma (pale blue under the sea), blue for stun,
 * yellow-green for acid. The harder it hits (the ammo's power), the bigger.
 * @param weapon The gun that fired.
 * @param firedAmmo The ammo the round fired, as the 3D view saw it leave (null if unknown).
 * @return The flash: palette colours (core, middle, rim), number of rays, how far and how
 *         sharply they spike out, and its size.
 */
FirstPersonMode::MuzzleFlash FirstPersonMode::muzzleFlashFor(const BattleItem *weapon, const RuleItem *firedAmmo) const
{
	// What the round fired, as the 3D view saw it leave (a launcher's only rocket is gone from
	// the launcher by now); else what's loaded; else the gun itself (it needs no ammo).
	const BattleItem *ammo = weapon->getAmmoForSlot(0);
	const RuleItem *shotRules = firedAmmo ? firedAmmo : ammo ? ammo->getRules() : weapon->getRules();
	const RuleDamageType *damageType = shotRules->getDamageType();
	const bool underwater = _save->getDepth() > 0;
	const SDL_Color *palette = _state->getPalette();

	// The look by damage type, in RGB colours: core, middle and rim; then how many rays, how
	// far they spike out (times the radius), how sharp they are, and the size.
	struct FlashLook { int core[3], mid[3], rim[3]; int rays; double spike, sharpness, size; };
	FlashLook look = { { 255, 250, 210 }, { 255, 200, 60 }, { 240, 110, 20 }, 4, 1.1, 1.5, 1.5 }; // bullets
	switch (damageType ? damageType->ResistType : DT_AP)
	{
	case DT_HE: look = { { 255, 240, 190 }, { 255, 150, 40 }, { 190, 60, 15 }, 8, 0.4, 1.0, 1.5 }; break;
	case DT_IN: look = { { 255, 220, 150 }, { 255, 110, 30 }, { 180, 30, 10 }, 6, 0.45, 1.0, 1.5 }; break;
	case DT_LASER:
		look = underwater ? FlashLook{ { 255, 255, 255 }, { 120, 230, 255 }, { 40, 140, 220 }, 8, 1.2, 6.0, 0.9 }
			: FlashLook{ { 255, 255, 255 }, { 255, 90, 70 }, { 200, 20, 20 }, 8, 1.2, 6.0, 0.9 };
		break;
	case DT_PLASMA:
		look = underwater ? FlashLook{ { 240, 255, 255 }, { 190, 240, 255 }, { 60, 150, 200 }, 5, 0.15, 1.0, 1.4 }
			: FlashLook{ { 230, 255, 220 }, { 110, 255, 110 }, { 30, 170, 60 }, 5, 0.15, 1.0, 1.4 };
		break;
	case DT_STUN: look = { { 230, 240, 255 }, { 150, 180, 255 }, { 70, 90, 220 }, 6, 0.6, 2.0, 1.3 }; break;
	case DT_ACID: look = { { 240, 255, 200 }, { 190, 255, 60 }, { 90, 170, 20 }, 5, 0.3, 1.0, 1.4 }; break;
	default: break;
	}

	// Into the palette's nearest colours, and sized by the power (50 is 1.0; between 0.8 and 1.5).
	MuzzleFlash flash;
	flash.core = nearestColor(palette, look.core[0], look.core[1], look.core[2]);
	flash.mid = nearestColor(palette, look.mid[0], look.mid[1], look.mid[2]);
	flash.rim = nearestColor(palette, look.rim[0], look.rim[1], look.rim[2]);
	flash.rays = look.rays;
	flash.spike = look.spike;
	flash.sharpness = look.sharpness;
	flash.size = look.size * std::clamp(shotRules->getPower() / 50.0, 0.8, 1.5);
	return flash;
}

/**
 * Draws the voxel weapon (the weapon in hand). It's placed exactly like the flat
 * plate (picture x, y mapped onto the plate, depth along the plate's normal at the same scale
 * as across it, see drawWeaponPlate), and drawn as its faces that look at the eye, far to near
 * (the painter's algorithm), each darkened by how far it turns from the light.
 * All positions are in camera space: x right, y down, z ahead, the eye at the origin.
 * @param nearEnd The stock end of the weapon's line.
 * @param along From the stock end to the muzzle end (its full length).
 * @param up Unit vector across the plate, "up" for the weapon.
 * @param normal Unit vector out of the plate (the picture's depth).
 * @param perPixel Camera-space length of one picture pixel.
 * @param shade How much darker the surroundings make it (0 bright .. 15 dark).
 */
void FirstPersonMode::drawWeaponVoxels(const double nearEnd[3], const double along[3], const double up[3], const double normal[3], double perPixel, int shade)
{
	const WeaponShape &shape = _heldShape;
	const double span = shape.muzzle - shape.stock; // picture pixels from stock to muzzle (signed)

	// The plate mapping as a linear map from picture space to camera space:
	// camera point = origin + x * stepPerX + y * stepPerY + depth * stepPerDepth,
	// for picture pixel (x, y) and a depth in voxels.
	double stepPerX[3], stepPerY[3], stepPerDepth[3], origin[3];
	for (int axis = 0; axis < 3; ++axis)
	{
		// One picture pixel along the weapon's axis, and one across it, in camera space.
		const double alongUnit = along[axis] / span, upUnit = shape.upSign * perPixel * up[axis];
		stepPerX[axis] = shape.along[0] * alongUnit + shape.across[0] * upUnit;
		stepPerY[axis] = shape.along[1] * alongUnit + shape.across[1] * upUnit;
		stepPerDepth[axis] = perPixel * normal[axis];

		// Where picture pixel (0, 0) lands: its along and across coordinates measured from the
		// stock end and the bore, which sit at nearEnd.
		const double alongAtZero = -shape.mean[0] * shape.along[0] - shape.mean[1] * shape.along[1];
		const double acrossAtZero = -shape.mean[0] * shape.across[0] - shape.mean[1] * shape.across[1];
		origin[axis] = nearEnd[axis] + (alongAtZero - shape.stock) * alongUnit + (acrossAtZero - shape.bore) * upUnit;
	}
	drawVoxelFaces(origin, stepPerX, stepPerY, stepPerDepth, shade);
}

/**
 * Draws the held voxel faces (_heldFaces): each corner goes from picture space to camera space
 * by the map given (camera point = origin + x * stepPerX + y * stepPerY + depth * stepPerDepth),
 * and the faces that look at the eye are drawn far to near (the painter's algorithm), each
 * darkened by how far it turns from the light.
 * All positions are in camera space: x right, y down, z ahead, the eye at the origin.
 * @param origin Where picture point (0, 0) at depth 0 lands.
 * @param stepPerX One picture pixel to the right, in camera space.
 * @param stepPerY One picture pixel down.
 * @param stepPerDepth One voxel of depth.
 * @param shade How much darker the surroundings make it (0 bright .. 15 dark).
 */
void FirstPersonMode::drawVoxelFaces(const double origin[3], const double stepPerX[3], const double stepPerY[3], const double stepPerDepth[3], int shade)
{
	// A mirror image (the weapon in the left hand) turns the faces' windings round: the sign of
	// the map's determinant tells, and flips the normals back.
	const double determinant = stepPerX[0] * (stepPerY[1] * stepPerDepth[2] - stepPerY[2] * stepPerDepth[1])
		- stepPerX[1] * (stepPerY[0] * stepPerDepth[2] - stepPerY[2] * stepPerDepth[0])
		+ stepPerX[2] * (stepPerY[0] * stepPerDepth[1] - stepPerY[1] * stepPerDepth[0]);
	const double windingSign = determinant < 0.0 ? -1.0 : 1.0;
	const double towardsLight[3] = { -0.42, -0.78, -0.46 }; // up, left, and from behind the eye

	// The weapon's lens: the view's centre and focal length in pixels (see WeaponLens).
	const int width = _weaponInHand->getWidth(), height = _weaponInHand->getHeight();
	const double focalPixels = width / 2.0 * WeaponLens, centreX = width / 2.0, centreY = height / 2.0;

	// Each face that shows, projected onto the screen with its shaded colour and its distance.
	struct Quad { double depth; double screen[4][2]; Uint8 color; };
	std::vector<Quad> quads;
	quads.reserve(_heldFaces.size() / 2);
	for (const VoxelFace &face : _heldFaces)
	{
		// The face's corners in camera space.
		double cameraCorners[4][3];
		for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
			for (int axis = 0; axis < 3; ++axis)
				cameraCorners[cornerIndex][axis] = origin[axis] + face.corner[cornerIndex][0] * stepPerX[axis]
					+ face.corner[cornerIndex][1] * stepPerY[axis] + face.corner[cornerIndex][2] * stepPerDepth[axis];

		// Its outward normal (from its first two edges) and its centre.
		double edge1[3], edge2[3], faceNormal[3], faceCentre[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			edge1[axis] = cameraCorners[1][axis] - cameraCorners[0][axis];
			edge2[axis] = cameraCorners[3][axis] - cameraCorners[0][axis];
			faceCentre[axis] = (cameraCorners[0][axis] + cameraCorners[2][axis]) * 0.5;
		}
		faceNormal[0] = (edge1[1] * edge2[2] - edge1[2] * edge2[1]) * windingSign;
		faceNormal[1] = (edge1[2] * edge2[0] - edge1[0] * edge2[2]) * windingSign;
		faceNormal[2] = (edge1[0] * edge2[1] - edge1[1] * edge2[0]) * windingSign;
		if (faceNormal[0] * faceCentre[0] + faceNormal[1] * faceCentre[1] + faceNormal[2] * faceCentre[2] >= 0.0 || faceCentre[2] < 0.1)
		{
			continue; // turned away from the eye, or behind it
		}

		// Shading: up to 3 shades darker as it turns from the light, plus the surroundings'
		// shade. Palette colours run in groups of 16 from bright to dark, so a shade is added
		// to the low four bits, staying inside the colour's group.
		const double normalLength = std::sqrt(faceNormal[0] * faceNormal[0] + faceNormal[1] * faceNormal[1] + faceNormal[2] * faceNormal[2]);
		const double lighting = std::max(0.0,
			(faceNormal[0] * towardsLight[0] + faceNormal[1] * towardsLight[1] + faceNormal[2] * towardsLight[2]) / normalLength);
		const int darkerBy = (int)std::lround((1.0 - lighting) * 3.0) + shade;
		Quad quad;
		quad.depth = faceCentre[0] * faceCentre[0] + faceCentre[1] * faceCentre[1] + faceCentre[2] * faceCentre[2]; // squared distance
		quad.color = (Uint8)((face.color & 0xF0) | std::min(15, (face.color & 15) + darkerBy));

		// Its corners on the screen (perspective projection through the weapon's lens).
		for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
		{
			quad.screen[cornerIndex][0] = centreX + cameraCorners[cornerIndex][0] / cameraCorners[cornerIndex][2] * focalPixels;
			quad.screen[cornerIndex][1] = centreY + cameraCorners[cornerIndex][1] / cameraCorners[cornerIndex][2] * focalPixels;
		}
		quads.push_back(quad);
	}

	// Paint them from the farthest to the nearest, so the near ones cover the far ones.
	std::sort(quads.begin(), quads.end(), [](const Quad &first, const Quad &second) { return first.depth > second.depth; });
	for (const Quad &quad : quads)
	{
		fillQuad(quad.screen, quad.color);
	}
}

/**
 * Fills a convex quadrilateral on the weapon surface: every pixel whose centre lies inside it,
 * row by row (a scanline fill).
 * @param corners The four corners on the screen, in pixels, in order around the edge.
 * @param color Palette colour to fill with.
 */
void FirstPersonMode::fillQuad(const double corners[4][2], Uint8 color)
{
	const int width = _weaponInHand->getWidth(), height = _weaponInHand->getHeight();

	// The rows it covers: those whose pixel centres lie between its top and bottom corners
	// (and on the surface).
	double top = corners[0][1], bottom = corners[0][1];
	for (int cornerIndex = 1; cornerIndex < 4; ++cornerIndex)
	{
		top = std::min(top, corners[cornerIndex][1]);
		bottom = std::max(bottom, corners[cornerIndex][1]);
	}
	const int firstRow = std::max(0, (int)std::ceil(top - 0.5)), lastRow = std::min(height - 1, (int)std::floor(bottom - 0.5));
	for (int y = firstRow; y <= lastRow; ++y)
	{
		// Where the row's centre line crosses the edges: the leftmost and rightmost crossing.
		// (Each edge includes its upper end but not its lower one, which also skips flat edges.)
		const double rowCentreY = y + 0.5;
		double leftX = 1e9, rightX = -1e9;
		for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
		{
			const double *edgeStart = corners[cornerIndex], *edgeEnd = corners[(cornerIndex + 1) % 4];
			if ((edgeStart[1] <= rowCentreY && edgeEnd[1] > rowCentreY) || (edgeEnd[1] <= rowCentreY && edgeStart[1] > rowCentreY))
			{
				const double crossX = edgeStart[0] + (rowCentreY - edgeStart[1]) / (edgeEnd[1] - edgeStart[1]) * (edgeEnd[0] - edgeStart[0]);
				leftX = std::min(leftX, crossX);
				rightX = std::max(rightX, crossX);
			}
		}

		// Fill the pixels whose centres lie between the two crossings.
		const int firstColumn = std::max(0, (int)std::ceil(leftX - 0.5)), lastColumn = std::min(width - 1, (int)std::floor(rightX - 0.5));
		for (int x = firstColumn; x <= lastColumn; ++x)
		{
			_weaponInHand->setPixel(x, y, color);
		}
	}
}

/**
 * Draws the weapon in hand, Doom style: its inventory picture on a plate in perspective, the
 * muzzle reaching in under the crosshair and the rest running off the bottom corner (right or
 * left, by the hand), with a little depth. It bobs with the steps, kicks back and up when it
 * fires, dips out of sight and back on a reload or a change of weapon, and swings in a melee
 * attack. The voxels are drawn instead of the plate (drawWeaponVoxels), placed the same way;
 * the flat plate is left only for a picture that gave no voxels.
 * Camera space here: x right, y down, z ahead, the eye at the origin; the screen point of a
 * camera point is its (x / z, y / z) times the weapon lens's focal length, from the centre.
 * @param left Held in the left hand (drawn mirrored, on the left).
 * @param bob The step bob (sideways, down), up to 4 each way.
 * @param kick The recoil kick, 0..1.
 * @param dip How far it's dipped down for a reload or a change of weapon, 0..70.
 * @param shade How much darker the surroundings make it (0 bright .. 15 dark).
 * @param tip Set to where the muzzle is on the canvas, in pixels (for the flash and the shots).
 */
void FirstPersonMode::drawWeaponPlate(bool left, double bob[2], double kick, double dip, int shade, double tip[2])
{
	const WeaponShape &shape = _heldShape;
	const int width = _weaponInHand->getWidth(), height = _weaponInHand->getHeight();
	const double focalPixels = width / 2.0 * WeaponLens, centreX = width / 2.0, centreY = height / 2.0;
	const double side = left ? -1.0 : 1.0; // mirrors x for the left hand

	// The muzzle is always at the same point on the screen, down and out towards the corner
	// (MuzzleScreen; the shot's flash and trail start there). The stock is on the same line from
	// the crosshair, 2.5 times further out (off the bottom corner), so the barrel points along a
	// ray at the crosshair whatever the weapon.
	// A melee swing pulls the weapon back and out (windup), then drives it in towards the
	// crosshair (strike): "reach" scales how far out on that line each end sits.
	double windup = 0.0, strike = 0.0;
	swingPhase(windup, strike);
	const double reachFar = 1.0 + 0.25 * windup - 0.85 * strike, reachNear = 1.0 + 0.1 * windup - 0.3 * strike;

	// The ends' depths: well back, so the weapon is mostly hidden. The recoil kicks it back
	// towards the eye; a strike brings it nearer too, and with its smaller reach the muzzle
	// swings in towards the crosshair.
	const double zFar = (6.9 - kick * 0.65) * (1.0 - 0.3 * strike), zNear = (1.95 - kick * 0.22) * (1.0 - 0.15 * strike);

	// The bob and the dip move both ends a little on the screen; the recoil and the wind-up also
	// lift the muzzle.
	const double sway[2] = { bob[0] * 0.002, bob[1] * 0.002 + dip * 0.004 };
	double farEnd[3] = { side * (MuzzleScreen[0] * reachFar + sway[0]) * zFar,
		(MuzzleScreen[1] * reachFar + sway[1] - kick * 0.03 - 0.05 * windup) * zFar, zFar };
	double nearEnd[3] = { side * (MuzzleScreen[0] * 2.5 * reachNear + sway[0]) * zNear, (MuzzleScreen[1] * 2.5 * reachNear + sway[1]) * zNear, zNear };

	// The weapon is a little shorter than that line, towards the muzzle (still on the same ray).
	for (int axis = 0; axis < 3; ++axis) nearEnd[axis] = farEnd[axis] + (nearEnd[axis] - farEnd[axis]) * WeaponScale;

	// A weapon shorter than the rifle shrinks about its grip (still on the line at the
	// crosshair), by its picture length's share of the rifle's to the power SizeCurve.
	const double sizeFactor = std::pow(std::clamp(std::abs(shape.muzzle - shape.stock) / RiflePixels, 0.4, 1.0), SizeCurve);
	for (int axis = 0; axis < 3; ++axis)
	{
		const double pivot = farEnd[axis] + (nearEnd[axis] - farEnd[axis]) * GripAt;
		farEnd[axis] = pivot + (farEnd[axis] - pivot) * sizeFactor;
		nearEnd[axis] = pivot + (nearEnd[axis] - pivot) * sizeFactor;
	}

	// A melee weapon is held tip up: the same line, turned about the grip. At rest it points up
	// (leaning a little forward and outwards); a strike turns it down onto the line at the
	// crosshair (where the thrust above makes it a jab) and it comes back up; the wind-up tilts
	// it a little further back first. One motion that reads as a swing and as a jab.
	if (shape.melee)
	{
		double pivot[3], towardsTip[3], farLength = 0.0, nearLength = 0.0;
		for (int axis = 0; axis < 3; ++axis)
		{
			pivot[axis] = farEnd[axis] + (nearEnd[axis] - farEnd[axis]) * GripAt;
			towardsTip[axis] = farEnd[axis] - pivot[axis];
			farLength += towardsTip[axis] * towardsTip[axis];
			nearLength += (nearEnd[axis] - pivot[axis]) * (nearEnd[axis] - pivot[axis]);
		}
		farLength = std::sqrt(farLength);
		nearLength = std::sqrt(nearLength);
		const double upright[3] = { side * MeleeUpright[0], MeleeUpright[1], MeleeUpright[2] };
		const double raise = std::clamp(1.0 - strike + 0.2 * windup, 0.0, 1.2);
		double direction[3], directionLength = 0.0;
		for (int axis = 0; axis < 3; ++axis)
		{
			direction[axis] = towardsTip[axis] / farLength * (1.0 - raise) + upright[axis] * raise;
			directionLength += direction[axis] * direction[axis];
		}
		directionLength = std::sqrt(directionLength);
		for (int axis = 0; axis < 3; ++axis)
		{
			farEnd[axis] = pivot[axis] + direction[axis] / directionLength * farLength;
			nearEnd[axis] = pivot[axis] - direction[axis] / directionLength * nearLength;
		}
	}

	// The weapon's axis from stock to muzzle, and its length in camera space.
	double along[3], length = 0.0;
	for (int axis = 0; axis < 3; ++axis) { along[axis] = farEnd[axis] - nearEnd[axis]; length += along[axis] * along[axis]; }
	length = std::sqrt(length);
	const double pixelsLong = std::max(4.0, std::abs(shape.muzzle - shape.stock));

	// Across (and in depth) the same scale as along: the weapon keeps its proportions.
	const double perPixel = length / pixelsLong;

	// "Up" across the plate: straight up, made square to the weapon's length (Gram-Schmidt).
	double up[3] = { 0.0, -1.0, 0.0 };
	const double upAlongShare = (up[0] * along[0] + up[1] * along[1] + up[2] * along[2]) / (length * length);
	for (int axis = 0; axis < 3; ++axis) up[axis] -= along[axis] * upAlongShare;
	const double upLength = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
	for (int axis = 0; axis < 3; ++axis) up[axis] /= upLength;

	// The plate's normal (the picture's depth): square to both, of unit length.
	double normal[3] = { along[1] * up[2] - along[2] * up[1], along[2] * up[0] - along[0] * up[2], along[0] * up[1] - along[1] * up[0] };
	const double normalLength = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
	for (int axis = 0; axis < 3; ++axis) normal[axis] /= normalLength;

	// The muzzle's tip on the screen, for the muzzle flash and our shots' starting point.
	tip[0] = centreX + farEnd[0] / farEnd[2] * focalPixels;
	tip[1] = centreY + farEnd[1] / farEnd[2] * focalPixels;

	// The plate's four corners on the screen bound the pixels to fill (the plate reaches as far
	// across from the bore as the picture does, either way; depth kept off the eye).
	const double acrossReach = std::max(std::abs(shape.minAcross - shape.bore), std::abs(shape.maxAcross - shape.bore)) * perPixel + 0.1;
	int minX = width, maxX = -1, minY = height, maxY = -1;
	for (int corner = 0; corner < 4; ++corner)
	{
		const double *endPoint = corner < 2 ? nearEnd : farEnd;
		const double acrossOffset = (corner & 1) ? acrossReach : -acrossReach;
		const double cornerPoint[3] = { endPoint[0] + up[0] * acrossOffset, endPoint[1] + up[1] * acrossOffset,
			std::max(0.2, endPoint[2] + up[2] * acrossOffset) };
		const double screenX = centreX + cornerPoint[0] / cornerPoint[2] * focalPixels, screenY = centreY + cornerPoint[1] / cornerPoint[2] * focalPixels;
		minX = std::min(minX, (int)screenX - 1); maxX = std::max(maxX, (int)screenX + 1);
		minY = std::min(minY, (int)screenY - 1); maxY = std::max(maxY, (int)screenY + 1);
	}
	minX = std::max(0, minX); minY = std::max(0, minY); maxX = std::min(width - 1, maxX); maxY = std::min(height - 1, maxY);

	// The voxel weapon, if this weapon has voxels (the flat plate below is only the fallback).
	if (!_heldFaces.empty())
	{
		drawWeaponVoxels(nearEnd, along, up, normal, perPixel, shade);
		return;
	}

	// The flat plate, in two layers: the far one, a little behind and 3 shades darker, shows
	// round the edges of the near one and gives it some body.
	const double thickness = 2.5 * perPixel;
	for (int layer = 0; layer < 2; ++layer)
	{
		// The layer's plane: through the stock end (pushed back for the far layer), facing
		// along the normal.
		const double layerOffset = layer == 0 ? thickness * side : 0.0;
		const double layerOrigin[3] = { nearEnd[0] + normal[0] * layerOffset, nearEnd[1] + normal[1] * layerOffset, nearEnd[2] + normal[2] * layerOffset };
		const double planeDistance = layerOrigin[0] * normal[0] + layerOrigin[1] * normal[1] + layerOrigin[2] * normal[2];
		const int layerShade = std::min(15, shade + (layer == 0 ? 3 : 0));

		// For each screen pixel in the bounds: the ray through its centre meets the plane at a
		// point; the point's place along the weapon and across it gives the picture pixel to
		// show there (a ray cast against the plate).
		for (int pixelY = minY; pixelY <= maxY; ++pixelY)
		{
			for (int pixelX = minX; pixelX <= maxX; ++pixelX)
			{
				// Where the ray meets the plane (not at all if it runs along it or away).
				const double ray[3] = { (pixelX + 0.5 - centreX) / focalPixels, (pixelY + 0.5 - centreY) / focalPixels, 1.0 };
				const double rayDotNormal = ray[0] * normal[0] + ray[1] * normal[1] + ray[2] * normal[2];
				if (std::abs(rayDotNormal) < 1e-6) continue;
				const double distanceAlongRay = planeDistance / rayDotNormal;
				if (distanceAlongRay <= 0.0) continue;

				// That point along the weapon (0 at the stock end, 1 at the muzzle) and across it
				// (picture pixels from the bore), then in picture coordinates along and across.
				const double fromOrigin[3] = { ray[0] * distanceAlongRay - layerOrigin[0], ray[1] * distanceAlongRay - layerOrigin[1],
					ray[2] * distanceAlongRay - layerOrigin[2] };
				const double alongFraction = (fromOrigin[0] * along[0] + fromOrigin[1] * along[1] + fromOrigin[2] * along[2]) / (length * length);
				if (alongFraction < 0.0 || alongFraction > 1.0) continue;
				const double acrossPixels = (fromOrigin[0] * up[0] + fromOrigin[1] * up[1] + fromOrigin[2] * up[2]) / perPixel;
				const double pictureAlong = shape.stock + alongFraction * (shape.muzzle - shape.stock), pictureAcross = shape.upSign * acrossPixels + shape.bore;
				if (pictureAcross < shape.minAcross - 0.5 || pictureAcross > shape.maxAcross + 0.5) continue;

				// The picture pixel there, shaded (within its colour group) and drawn.
				const int spriteX = (int)std::floor(shape.mean[0] + pictureAlong * shape.along[0] + pictureAcross * shape.across[0] + 0.5);
				const int spriteY = (int)std::floor(shape.mean[1] + pictureAlong * shape.along[1] + pictureAcross * shape.across[1] + 0.5);
				if (spriteX < 0 || spriteY < 0 || spriteX >= _heldSprite->getWidth() || spriteY >= _heldSprite->getHeight()) continue;
				const Uint8 color = _heldSprite->getPixel(spriteX, spriteY);
				if (color) _weaponInHand->setPixel(pixelX, pixelY, (Uint8)((color & 0xF0) | std::min(15, (color & 15) + layerShade)));
			}
		}
	}
}

/**
 * Draws anything that isn't a gun or a club (a grenade, a medi-kit, a scanner...) as its voxels
 * (its inventory picture given volume): in the bottom corner with its lower half below the
 * edge of the screen (so no hand needs drawing), turned so its depth shows (HeldItemYaw,
 * HeldItemPitch, HeldItemRoll), and shaded face by face like the weapon. A picture that gave no
 * voxels is drawn flat instead, three times as big.
 * @param left Held in the left hand: drawn bottom left, clear of the info panel.
 * @param bob The step bob, in canvas pixels (sideways, down).
 * @param dip How far it's dipped down for a change of item, in canvas pixels.
 * @param shade How much darker the surroundings make it (0 bright .. 15 dark).
 */
void FirstPersonMode::drawHeldItem(bool left, double bob[2], double dip, int shade)
{
	if (!_heldFaces.empty())
	{
		// Its size: HeldItemScale canvas pixels per picture pixel on a 200-line canvas, in
		// proportion on bigger ones, and a small picture (a grenade) as big as one of
		// HeldItemMinPixels; the bob and the dip grow with the canvas.
		const WeaponShape &shape = _heldShape;
		const int width = _weaponInHand->getWidth(), height = _weaponInHand->getHeight();
		const double boxWidth = shape.boxMax[0] - shape.boxMin[0] + 1.0, boxHeight = shape.boxMax[1] - shape.boxMin[1] + 1.0;
		const double canvasScale = height / 200.0;
		const double scale = HeldItemScale * canvasScale * std::max(1.0, HeldItemMinPixels / std::max(boxWidth, boxHeight));

		// Where the middle of its box goes on the canvas: in the corner (the left hand's clear of
		// the info panel), on the bottom edge, so the lower half is out of sight.
		const double middleX = (left ? PanelW + 10 + boxWidth * scale / 2.0 : width - boxWidth * scale / 2.0 - 12.0 * canvasScale) + bob[0] * canvasScale;
		const double middleY = height + (bob[1] + dip) * canvasScale;

		// That point in camera space, one unit ahead of the eye (through the weapon's lens), and
		// the camera-space length of one picture pixel there.
		const double focalPixels = width / 2.0 * WeaponLens;
		const double middle[3] = { (middleX - width / 2.0) / focalPixels, (middleY - height / 2.0) / focalPixels, 1.0 };
		const double perPixel = scale / focalPixels;

		// Turned: leaning in (about the view's axis), its top tipped back (about the sideways
		// axis), then towards the screen's centre (about the upright axis); the left hand mirrors
		// the lean and the turn. The columns of the turn are where the picture's right, down and
		// depth end up.
		const double degrees = Pi / 180.0;
		const double yaw = (left ? -HeldItemYaw : HeldItemYaw) * degrees, pitch = HeldItemPitch * degrees, roll = (left ? -HeldItemRoll : HeldItemRoll) * degrees;
		const double rollTurn[3][3] = { { std::cos(roll), -std::sin(roll), 0 }, { std::sin(roll), std::cos(roll), 0 }, { 0, 0, 1 } };
		const double pitchTurn[3][3] = { { 1, 0, 0 }, { 0, std::cos(pitch), -std::sin(pitch) }, { 0, std::sin(pitch), std::cos(pitch) } };
		const double yawTurn[3][3] = { { std::cos(yaw), 0, std::sin(yaw) }, { 0, 1, 0 }, { -std::sin(yaw), 0, std::cos(yaw) } };
		double pitchRoll[3][3], turn[3][3];
		for (int row = 0; row < 3; ++row)
			for (int column = 0; column < 3; ++column)
				pitchRoll[row][column] = pitchTurn[row][0] * rollTurn[0][column] + pitchTurn[row][1] * rollTurn[1][column] + pitchTurn[row][2] * rollTurn[2][column];
		for (int row = 0; row < 3; ++row)
			for (int column = 0; column < 3; ++column)
				turn[row][column] = yawTurn[row][0] * pitchRoll[0][column] + yawTurn[row][1] * pitchRoll[1][column] + yawTurn[row][2] * pitchRoll[2][column];

		// The map from picture space: the box's middle lands on that point.
		const double boxMiddleX = (shape.boxMin[0] + shape.boxMax[0]) / 2.0, boxMiddleY = (shape.boxMin[1] + shape.boxMax[1]) / 2.0;
		double stepPerX[3], stepPerY[3], stepPerDepth[3], origin[3];
		for (int axis = 0; axis < 3; ++axis)
		{
			stepPerX[axis] = turn[axis][0] * perPixel;
			stepPerY[axis] = turn[axis][1] * perPixel;
			stepPerDepth[axis] = turn[axis][2] * perPixel;
			origin[axis] = middle[axis] - boxMiddleX * stepPerX[axis] - boxMiddleY * stepPerY[axis];
		}
		drawVoxelFaces(origin, stepPerX, stepPerY, stepPerDepth, shade);
		return;
	}

	// The flat picture: where its top left corner goes on the canvas.
	const int scale = 3, width = _weaponInHand->getWidth(), height = _weaponInHand->getHeight();
	const int spriteWidth = _heldSprite->getWidth(), spriteHeight = _heldSprite->getHeight();
	const int itemX = (left ? PanelW + 10 : width - spriteWidth * scale - 12) + (int)bob[0]; // the left hand's clear of the info panel
	const int itemY = height - spriteHeight * scale * 6 / 10 + (int)(bob[1] + dip);

	// Each canvas pixel of the magnified picture, shaded (within its colour group), if it's on
	// the canvas.
	for (int y = 0; y < spriteHeight * scale; ++y)
	{
		for (int x = 0; x < spriteWidth * scale; ++x)
		{
			const Uint8 color = _heldSprite->getPixel(x / scale, y / scale);
			if (color && itemX + x >= 0 && itemY + y >= 0 && itemX + x < width && itemY + y < height)
				_weaponInHand->setPixel(itemX + x, itemY + y, (Uint8)((color & 0xF0) | std::min(15, (color & 15) + shade)));
		}
	}
}

/**
 * Is a weapon drawn in the soldier's hands? Not a built-in one: an item fixed to the unit (a
 * tank's cannon, an alien's claws) or the unit's special weapon (OXCE's, used from the SPECIAL
 * button or by an empty hand: fists, a dog's bite, a Lamia's spit). A tank or a machine has no
 * hands, and a bite has nothing to hold: the view's hands stay empty. A mod can name built-in
 * items drawn all the same (`firstPerson: builtInItemsShown:`, a shield, an arm cannon).
 * Aiming and firing work as with any weapon; only the picture is left out.
 * @param unit The soldier.
 * @param weapon The weapon in use.
 */
bool FirstPersonMode::weaponShownInHand(BattleUnit *unit, const BattleItem *weapon) const
{
	const RuleItem *rules = weapon->getRules();
	const bool held = weapon == unit->getLeftHandWeapon() || weapon == unit->getRightHandWeapon();
	const bool builtIn = rules->isFixed() || !held;
	return !builtIn || _game->getMod()->getFirstPerson().showsBuiltInItem(rules->getType());
}

/**
 * Updates and draws the weapon in the soldier's hand, every frame, at the bottom of the screen
 * like an FPS (setting "Weapon in hand"), shaded by the game's light where the soldier stands.
 * Notices a new weapon (its picture is measured again) or a reload (it dips out of sight and
 * back), our own shots (a muzzle flash), and with dual wield draws the other hand's gun too.
 * Tells the 3D view where each muzzle is on the screen, so our shots start from there.
 * @param unit The soldier being looked through (null: nothing is drawn).
 */
void FirstPersonMode::updateWeaponInHand(BattleUnit *unit)
{
	// Shown only in the 3D view with the setting on, the classic HUD off, a weapon in hand,
	// nobody going down, and the game's inventory pictures (BIGOBS.PCK) at hand.
	// (A built-in weapon with nothing to hold leaves the hands empty: see weaponShownInHand.)
	BattleItem *weapon = unit ? activeWeapon(unit) : nullptr;
	SurfaceSet *inventorySprites = _game->getMod()->getSurfaceSet("BIGOBS.PCK");
	const bool show = Options::fpsViewModel && isActive() && !_classicHud && weapon && weaponShownInHand(unit, weapon) && !_deathUnit && inventorySprites;
	_weaponInHand->setVisible(show);
	_view->setMuzzlePoint(0.0, 0.0, false); // set again below while a gun is in hand
	_view->setOffMuzzlePoint(0.0, 0.0, false);
	if (!show)
	{
		_heldItem = nullptr;
		return;
	}

	// A reload (another clip in the gun than last frame, also into a gun whose spent clip the
	// game took out) or another weapon: dip out of sight and back (not on the first frame, when
	// nothing was held before).
	const Uint32 now = SDL_GetTicks();
	const BattleItem *ammo = weapon->getAmmoForSlot(0);
	const int clipId = ammo && ammo != weapon ? ammo->getId() : -1; // -1: no clip (or none needed)
	if (weapon != _heldItem || (clipId != -1 && clipId != _heldClipId))
	{
		_heldDipStart = _heldItem ? now : 0;
	}
	_heldItem = weapon;
	_heldClipId = clipId;

	// A different kind of weapon: draw its inventory picture (the game's own hand sprite) and
	// measure it again. Guns and clubs are drawn pointing at the crosshair, the rest held up.
	const BattleType battleType = weapon->getRules()->getBattleType();
	if (weapon->getRules() != _heldRules)
	{
		_heldRules = weapon->getRules();
		_heldSprite->clear();
		weapon->getRules()->drawHandSprite(inventorySprites, _heldSprite, weapon, _save, 0);
		analyzeWeapon(battleType == BT_FIREARM || battleType == BT_MELEE, battleType == BT_MELEE);
	}

	// The game's shade where the eye is (at most 9 shades darker, so it stays visible in the
	// dark), the hand, and with dual wield the other hand's gun (drawn on the other side).
	const int shade = std::min(9, _view->getEyeShade());
	const bool inLeftHand = handOf(unit, weapon) == 1;
	BattleItem *otherWeapon = offHandWeapon(unit, weapon);

	// The bob with the steps: _headBobPhase counts the voxels walked, so the weapon sways
	// sideways once every 16 voxels and bounces down twice in that time (in time with the head
	// bob), by up to 4, fading in and out as the soldier starts and stops walking (only with the
	// head bob setting).
	const double bobAngle = _headBobPhase * 2.0 * Pi / 16.0;
	double bob[2] = { std::sin(bobAngle) * 4.0 * _headBobAmount, std::abs(std::cos(bobAngle)) * 4.0 * _headBobAmount };

	// A new round of ours left the muzzle (the 3D view stamps each with its time): a muzzle
	// flash for 80 ms, its look by what was fired.
	if (_view->getOwnShotTick() != _heldShotSeen)
	{
		_heldShotSeen = _view->getOwnShotTick();
		if (battleType == BT_FIREARM)
		{
			_muzzleFlashUntil = now + 80;
			_muzzleFlash = muzzleFlashFor(weapon, _view->getOwnShotAmmo());
		}
	}

	// Dual wield: the other gun fired with it, so it flashes too.
	if (_view->getPartnerShotTick() != _offHandShotSeen)
	{
		_offHandShotSeen = _view->getPartnerShotTick();
		if (otherWeapon)
		{
			_offHandFlashUntil = now + 80;
			_offHandFlash = muzzleFlashFor(otherWeapon, _view->getPartnerShotAmmo());
		}
	}

	// The recoil kick (full at 4 degrees of the view's kick), and the dip: down by up to 70
	// (about canvas pixels) and back up over 600 ms, along half a sine wave.
	const double kick = std::min(1.0, _view->getRecoilKick() / 4.0);
	double dip = 0.0;
	if (_heldDipStart && now - _heldDipStart < 600)
	{
		dip = std::sin(Pi * (now - _heldDipStart) / 600.0) * 70.0;
	}
	// Looking far down, the weapon slides gently down out of sight (eased in from 45 to 72
	// degrees below level, following the view), and back up as the view comes up: held low,
	// it would cover the ground at the soldier's feet. Aiming and firing don't change.
	// Zoomed in, it's lowered out of sight the same way, as far as the zoom has gone.
	const double lookingDown = -_view->getPitchDegrees();
	const double lookDownShare = std::clamp((lookingDown - LookDownSlideFrom) / (LookDownSlideTo - LookDownSlideFrom), 0.0, 1.0);
	const double zoomShare = std::log(_zoom) / std::log(ZoomFactor);
	const double slideShare = std::max(lookDownShare, zoomShare);
	if (slideShare > 0.0)
	{
		dip += slideShare * slideShare * (3.0 - 2.0 * slideShare) * _weaponInHand->getHeight() * 0.8;
	}
	_weaponInHand->clear();
	_weaponInHand->lock();

	// A muzzle flash at a tip, behind the weapon (with the setting "Muzzle flashes"): a disc
	// with rays that spin, its core, middle and rim in their own colours.
	auto drawFlash = [&](const double tip[2], const MuzzleFlash &flash)
	{
		// Its radius in pixels (by the canvas width and the flash's size), and how far the rays
		// reach at most.
		const int radius = std::max(5, (int)std::lround(_weaponInHand->getWidth() / 30.0 * flash.size));
		const int reach = (int)std::ceil(radius * (1.0 + flash.spike));

		// Centred just ahead of the bore, along the barrel towards the crosshair (the screen's
		// centre): the muzzle covers the back of it.
		const double toCentreX = _weaponInHand->getWidth() / 2.0 - tip[0], toCentreY = _weaponInHand->getHeight() / 2.0 - tip[1];
		const double toCentreLength = std::max(1.0, std::sqrt(toCentreX * toCentreX + toCentreY * toCentreY));
		const int flashX = (int)std::lround(tip[0] + toCentreX / toCentreLength * radius * 0.5),
			flashY = (int)std::lround(tip[1] + toCentreY / toCentreLength * radius * 0.5);
		const Uint8 core = flash.core, mid = flash.mid, rim = flash.rim;
		for (int offsetY = -reach; offsetY <= reach; ++offsetY)
		{
			for (int offsetX = -reach; offsetX <= reach; ++offsetX)
			{
				// The edge at this angle: the radius, pushed out where a ray points (the rays
				// turn with time; how many, how far and how sharp is the flash's look).
				const double distance = std::sqrt((double)(offsetX * offsetX + offsetY * offsetY)), angle = std::atan2((double)offsetY, (double)offsetX);
				const double rayStrength = std::pow(std::max(0.0, std::cos(angle * flash.rays + now * 0.05)), flash.sharpness);
				const double edgeRadius = radius * (1.0 + flash.spike * rayStrength);
				if (distance > edgeRadius) continue;

				// Only on the canvas, and only where the weapon isn't (it shows round the muzzle).
				const int pixelX = flashX + offsetX, pixelY = flashY + offsetY;
				if (pixelX < 0 || pixelY < 0 || pixelX >= _weaponInHand->getWidth() || pixelY >= _weaponInHand->getHeight()) continue;
				if (_weaponInHand->getPixel(pixelX, pixelY)) continue;

				// The inner 35% the core colour, out to 70% the middle one, the rest the rim.
				_weaponInHand->setPixel(pixelX, pixelY, distance < edgeRadius * 0.35 ? core : distance < edgeRadius * 0.7 ? mid : rim);
			}
		}
	};

	if (_heldShape.gun)
	{
		// A gun or a club, pointing at the crosshair. Our shots leave its muzzle (the canvas
		// point scaled to the 3D view's pixels), and it flashes when it fires.
		double tip[2];
		drawWeaponPlate(inLeftHand, bob, kick, dip, shade, tip);
		const double canvasToView = _view->getWidth() / (double)_weaponInHand->getWidth();
		_view->setMuzzlePoint(tip[0] * canvasToView, tip[1] * canvasToView, true);
		if (Options::fpsMuzzleFlash && now < _muzzleFlashUntil)
		{
			drawFlash(tip, _muzzleFlash);
		}

		// Dual wield: the other gun, on the other side, swapped in for drawing (its picture is
		// drawn and measured again when it's a different kind); its rounds leave its muzzle.
		if (otherWeapon)
		{
			swapOffHand();
			if (otherWeapon->getRules() != _heldRules)
			{
				_heldRules = otherWeapon->getRules();
				_heldSprite->clear();
				otherWeapon->getRules()->drawHandSprite(inventorySprites, _heldSprite, otherWeapon, _save, 0);
				analyzeWeapon(true);
			}
			if (_heldShape.gun)
			{
				double otherTip[2];
				drawWeaponPlate(!inLeftHand, bob, kick, dip, shade, otherTip);
				_view->setOffMuzzlePoint(otherTip[0] * canvasToView, otherTip[1] * canvasToView, true);
				if (Options::fpsMuzzleFlash && now < _offHandFlashUntil)
				{
					drawFlash(otherTip, _offHandFlash);
				}
			}
			swapOffHand();
		}
	}
	else
	{
		// Anything else (a grenade, a medi-kit...), held up in the corner.
		drawHeldItem(inLeftHand, bob, dip, shade);
	}
	_weaponInHand->unlock();
}

}

// Thanks mom and dad! You were the ones who exposed me to my first comp.
