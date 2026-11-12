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
 * FirstPersonPresent.cpp: a 3D view at the display's own size (setting "3D resolution": Display).
 *
 * The game draws everything onto a screen of its own, the battlescape's size (often a half or a
 * quarter of the display's), which the engine then enlarges onto the display. A 3D view drawn
 * into that screen can never be finer than it. So a view the display's size isn't drawn into it
 * at all: the mode fills the screen with a key value where the view is to show (FirstPersonMode::blit); and when the engine shows the frame, every
 * pixel of the display where the screen still holds the key takes the view's pixel. The game's
 * windows and the mouse pointer keep the screen's scale, on top; the view has the display's.
 * The mode's own HUD goes the same way as the view: its canvas is laid out for the display at a
 * scale of its own and put over the view in one display-sized picture, and that picture is the
 * "view" handed over here (FirstPersonMode::blit, canvasTarget). This is also where a picture in more colours than the palette's would go to the
 * display one day: it is the one place the 3D picture meets the display.
 *
 * The key: on an 8-bit screen, colour 0, which nothing draws (it is every surface's transparent
 * colour). On a 32-bit one (the engine's HQX and xBRZ scalers, and its OpenGL output), a blue
 * one step off black, which no palette colour is.
 *
 * The engine shows a frame in one of two ways, each with its hook:
 *  - itself (Screen::flip): the screen enlarged into the display's own pixels; the view's are
 *    put in after (presentAtFlip);
 *  - through OpenGL (Zoom::flipWithZoom): the screen as a texture drawn over the window; the
 *    view is drawn over that as a second texture, see-through where the screen has anything
 *    (presentAtFlipOpenGl).
 */
#include "FirstPersonApi.h"
#include <vector>
#include <SDL.h>
#include "../Engine/Screen.h"
#include "../Engine/Surface.h"
#ifndef __NO_OPENGL
#include <SDL_opengl.h>
#endif

namespace OpenXcom
{

namespace FirstPerson
{

namespace
{
const Surface *viewToPresent = nullptr; // the view asked for this frame
const Uint32 *fullToPresent = nullptr;  // ... and its pixels in full colour (0xFFRRGGBB a pixel; 0: the palette's colour), or null
}

bool canPresentAtDisplaySize()
{
	return true;
}

Uint32 presentKey(SDL_Surface *screen)
{
	return screen->format->BytesPerPixel == 1 ? 0 : SDL_MapRGB(screen->format, 0, 0, 1);
}

void presentView(const Surface *view, const Uint32 *fullColors)
{
	viewToPresent = view;
	fullToPresent = fullColors;
}

/**
 * Lays the view asked for this frame into the display's own pixels: where the display's picture
 * area still holds the key (nothing of the HUD was drawn there), the view's pixel, through the
 * palette on a 32-bit display. Nothing if no view asked, or if it isn't the picture area's size
 * (the display just changed: the next frame's view will be).
 */
void presentAtFlip(SDL_Surface *display, int x, int y, int width, int height)
{
	const Surface *view = viewToPresent;
	const Uint32 *fullColors = fullToPresent;
	viewToPresent = nullptr;
	fullToPresent = nullptr;
	if (!view || !display || view->getWidth() != width || view->getHeight() != height || x < 0 || y < 0 || x + width > display->w || y + height > display->h)
	{
		return;
	}
	const int bytes = display->format->BytesPerPixel;
	if (bytes != 1 && bytes != 4)
	{
		return;
	}
	if (SDL_MUSTLOCK(display) && SDL_LockSurface(display) != 0)
	{
		return;
	}
	if (bytes == 1)
	{
		for (int row = 0; row < height; ++row)
		{
			Uint8 *shown = (Uint8 *)display->pixels + (size_t)(y + row) * display->pitch + x;
			const Uint8 *seen = view->getRaw(0, row);
			for (int column = 0; column < width; ++column)
			{
				if (!shown[column]) shown[column] = seen[column];
			}
		}
	}
	else
	{
		// The palette as the display's pixels, and the key by its colour alone (whatever the display keeps in its spare byte).
		Uint32 colors[256];
		const SDL_Color *palette = view->getPalette();
		for (int index = 0; index < 256; ++index)
		{
			colors[index] = SDL_MapRGB(display->format, palette[index].r, palette[index].g, palette[index].b);
		}
		const Uint32 colorBits = display->format->Rmask | display->format->Gmask | display->format->Bmask;
		const Uint32 key = SDL_MapRGB(display->format, 0, 0, 1) & colorBits;
		// (The full colours are red, green, blue from the top byte down: most displays' own order; else each is moved to its place.)
		const SDL_PixelFormat *format = display->format;
		const bool sameOrder = format->Rshift == 16 && format->Gshift == 8 && format->Bshift == 0;
		for (int row = 0; row < height; ++row)
		{
			Uint32 *shown = (Uint32 *)((Uint8 *)display->pixels + (size_t)(y + row) * display->pitch) + x;
			const Uint8 *seen = view->getRaw(0, row);
			const Uint32 *full = fullColors ? fullColors + (size_t)row * width : nullptr;
			for (int column = 0; column < width; ++column)
			{
				if ((shown[column] & colorBits) != key) continue;
				const Uint32 fullColor = full ? full[column] : 0;
				shown[column] = !fullColor ? colors[seen[column]] : sameOrder ? (fullColor & 0xFFFFFFu)
					: ((fullColor >> 16) & 255u) << format->Rshift | ((fullColor >> 8) & 255u) << format->Gshift | (fullColor & 255u) << format->Bshift;
			}
		}
	}
	if (SDL_MUSTLOCK(display)) SDL_UnlockSurface(display);
}

/**
 * The same with the engine's OpenGL output: it has just drawn the game's screen over the window
 * as a texture; the view goes over that as a texture of its own, the display's size, opaque
 * where the screen holds the key and see-through elsewhere (worked out here from the screen's
 * own pixels: the nearest one to each of the display's). Drawn with OpenGL's oldest calls, as
 * the engine draws; the texture the engine had bound is bound again after.
 * @param screen The game's screen as the engine hands it to OpenGL (32-bit).
 * @param x, y, width, height The picture area on the display, from its top left.
 * @param displayHeight The display's height (OpenGL counts from the bottom).
 */
void presentAtFlipOpenGl(SDL_Surface *screen, int x, int y, int width, int height, int displayHeight)
{
#ifndef __NO_OPENGL
	const Surface *view = viewToPresent;
	const Uint32 *fullColors = fullToPresent;
	viewToPresent = nullptr;
	fullToPresent = nullptr;
	if (!view || !screen || screen->format->BytesPerPixel != 4 || view->getWidth() != width || view->getHeight() != height || width <= 0 || height <= 0)
	{
		return;
	}
	// The view's picture as OpenGL takes it (blue, green, red, opacity: a byte each).
	static std::vector<Uint32> picture;
	static std::vector<int> screenColumn;
	picture.resize((size_t)width * height);
	screenColumn.resize(width);
	for (int column = 0; column < width; ++column) screenColumn[column] = (int)((long long)column * screen->w / width);
	Uint32 colors[256];
	const SDL_Color *palette = view->getPalette();
	for (int index = 0; index < 256; ++index)
	{
		colors[index] = 0xFF000000u | (Uint32)palette[index].r << 16 | (Uint32)palette[index].g << 8 | palette[index].b;
	}
	const Uint32 colorBits = screen->format->Rmask | screen->format->Gmask | screen->format->Bmask;
	const Uint32 key = SDL_MapRGB(screen->format, 0, 0, 1) & colorBits;
	for (int row = 0; row < height; ++row)
	{
		const Uint32 *screenRow = (const Uint32 *)((const Uint8 *)screen->pixels + (size_t)((long long)row * screen->h / height) * screen->pitch);
		const Uint8 *seen = view->getRaw(0, row);
		Uint32 *made = &picture[(size_t)row * width];
		const Uint32 *full = fullColors ? fullColors + (size_t)row * width : nullptr;
		for (int column = 0; column < width; ++column)
		{
			made[column] = (screenRow[screenColumn[column]] & colorBits) != key ? 0 : full && full[column] ? full[column] : colors[seen[column]];
		}
	}

	// Its texture (made again when the size changes, or when the display was made anew and took the old one with it).
	static GLuint texture = 0;
	static int textureWidth = 0, textureHeight = 0;
	GLint boundBefore = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundBefore);
	if (!texture || !glIsTexture(texture) || textureWidth != width || textureHeight != height)
	{
		if (texture && glIsTexture(texture)) glDeleteTextures(1, &texture);
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, picture.data());
		textureWidth = width;
		textureHeight = height;
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, texture);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, picture.data());
	}
	// Over the picture area (the engine left the window's pixels as the drawing's units, counted from the bottom left).
	const int left = x, right = x + width, top = displayHeight - y, bottom = displayHeight - y - height;
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glBegin(GL_TRIANGLE_STRIP);
	glTexCoord2f(0, 0); glVertex3i(left, top, 0);
	glTexCoord2f(1, 0); glVertex3i(right, top, 0);
	glTexCoord2f(0, 1); glVertex3i(left, bottom, 0);
	glTexCoord2f(1, 1); glVertex3i(right, bottom, 0);
	glEnd();
	glDisable(GL_BLEND);
	glBindTexture(GL_TEXTURE_2D, (GLuint)boundBefore);
	while (glGetError() != GL_NO_ERROR); // (nothing of ours may trip the engine's own checks)
#else
	(void)screen; (void)x; (void)y; (void)width; (void)height; (void)displayHeight;
	viewToPresent = nullptr;
	fullToPresent = nullptr;
#endif
}

}

}
