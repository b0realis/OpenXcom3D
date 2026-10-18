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
#include "FirstPersonParticles.h"
#include <algorithm>
#include <cmath>

namespace OpenXcom
{

FirstPersonParticles::FirstPersonParticles() : _seed(0x9E3779B9u)
{
	_particles.reserve(Max);
}

void FirstPersonParticles::add(const Particle &p)
{
	if (_particles.size() < Max)
	{
		_particles.push_back(p);
	}
}

float FirstPersonParticles::random()
{
	_seed ^= _seed << 13;
	_seed ^= _seed >> 17;
	_seed ^= _seed << 5;
	return (_seed & 0xFFFFFF) / 16777216.0f;
}

/**
 * Moves particles along (gravity, drag) and removes the expired ones.
 */
void FirstPersonParticles::update(float dt)
{
	for (auto &p : _particles)
	{
		p.age += dt;
		const float damping = std::max(0.0f, 1.0f - p.drag * dt);
		p.vel[2] -= p.gravity * dt;
		for (int a = 0; a < 3; ++a)
		{
			p.vel[a] *= damping;
			p.pos[a] += p.vel[a] * dt;
		}
	}
	_particles.erase(std::remove_if(_particles.begin(), _particles.end(), [](const Particle &p) { return p.age >= p.life; }), _particles.end());
}

void FirstPersonParticles::cull(Group group, float x, float y, float radius)
{
	const float r2 = radius * radius;
	_particles.erase(std::remove_if(_particles.begin(), _particles.end(), [=](const Particle &p)
	{
		const float dx = p.pos[0] - x, dy = p.pos[1] - y;
		return p.group == group && dx * dx + dy * dy > r2;
	}), _particles.end());
}

int FirstPersonParticles::count(Group group) const
{
	return (int)std::count_if(_particles.begin(), _particles.end(), [group](const Particle &p) { return p.group == group; });
}

/**
 * Throws particles out of a point in random directions (biased upwards), with random
 * speed, colour and lifetime.
 */
void FirstPersonParticles::burst(const float pos[3], int count, Kind kind, Group group, const Uint8 *colors, int colorCount,
	float speedMin, float speedMax, float upBias, float gravity, float drag, float lifeMin, float lifeMax, Uint8 size)
{
	for (int i = 0; i < count; ++i)
	{
		Particle p;
		const float yaw = random(0.0f, 6.2831853f), z = random(-1.0f, 1.0f);
		const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
		const float speed = random(speedMin, speedMax);
		const float dir[3] = { r * std::cos(yaw), r * std::sin(yaw), z + upBias };
		for (int a = 0; a < 3; ++a)
		{
			p.pos[a] = pos[a];
			p.vel[a] = dir[a] * speed;
		}
		p.age = 0.0f;
		p.life = random(lifeMin, lifeMax);
		p.gravity = gravity;
		p.drag = drag;
		p.color = colors[(int)(random() * colorCount) % colorCount];
		p.size = size;
		p.kind = kind;
		p.group = group;
		add(p);
	}
}

}
