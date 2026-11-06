/**************************************************************************/
/*  sun_tile_mask.h                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

// lestoroer#sun: Какие плитки карты солнца устарели. Прямоугольники — в плитках, правая и нижняя границы не входят.

#include "core/math/rect2i.h"
#include "core/typedefs.h"

class SunTileMask {
public:
	static constexpr int SIZE = 64;

private:
	uint64_t rows[SIZE] = {};

	static uint64_t row_bits(int p_from, int p_count) {
		const uint64_t bits = p_count >= SIZE ? ~uint64_t(0) : (uint64_t(1) << p_count) - 1;
		return bits << p_from;
	}

public:
	void set_all() {
		for (uint64_t &row : rows) {
			row = ~uint64_t(0);
		}
	}

	void set_rect(const Rect2i &p_tiles) {
		if (p_tiles.size.x <= 0 || p_tiles.size.y <= 0) {
			return;
		}
		const uint64_t bits = row_bits(p_tiles.position.x, p_tiles.size.x);
		for (int y = p_tiles.position.y; y < p_tiles.position.y + p_tiles.size.y; y++) {
			rows[y] |= bits;
		}
	}

	void clear_rect(const Rect2i &p_tiles) {
		if (p_tiles.size.x <= 0 || p_tiles.size.y <= 0) {
			return;
		}
		const uint64_t bits = row_bits(p_tiles.position.x, p_tiles.size.x);
		for (int y = p_tiles.position.y; y < p_tiles.position.y + p_tiles.size.y; y++) {
			rows[y] &= ~bits;
		}
	}

	bool any() const {
		for (const uint64_t row : rows) {
			if (row != 0) {
				return true;
			}
		}
		return false;
	}

	void reset() {
		for (uint64_t &row : rows) {
			row = 0;
		}
	}

	void clear_mask(const SunTileMask &p_mask) {
		for (int y = 0; y < SIZE; y++) {
			rows[y] &= ~p_mask.rows[y];
		}
	}

	void keep_mask(const SunTileMask &p_mask) {
		for (int y = 0; y < SIZE; y++) {
			rows[y] &= p_mask.rows[y];
		}
	}

	void keep_rect(const Rect2i &p_tiles) {
		const uint64_t bits = row_bits(p_tiles.position.x, p_tiles.size.x);
		for (int y = 0; y < SIZE; y++) {
			rows[y] &= (y >= p_tiles.position.y && y < p_tiles.position.y + p_tiles.size.y) ? bits : uint64_t(0);
		}
	}

	bool intersects(const Rect2i &p_tiles) const {
		if (p_tiles.size.x <= 0 || p_tiles.size.y <= 0) {
			return false;
		}
		const uint64_t bits = row_bits(p_tiles.position.x, p_tiles.size.x);
		for (int y = p_tiles.position.y; y < p_tiles.position.y + p_tiles.size.y; y++) {
			if (rows[y] & bits) {
				return true;
			}
		}
		return false;
	}

	// Охватывающий прямоугольник плиток, отмеченных и здесь, и в другой маске; пустой, если таких нет.
	Rect2i bounds_within(const SunTileMask &p_mask) const {
		int min_x = SIZE, max_x = -1, min_y = SIZE, max_y = -1;
		for (int y = 0; y < SIZE; y++) {
			uint64_t row = rows[y] & p_mask.rows[y];
			if (row == 0) {
				continue;
			}
			min_y = MIN(min_y, y);
			max_y = y;
			int x = 0;
			while (row != 0) {
				if (row & 1) {
					min_x = MIN(min_x, x);
					max_x = MAX(max_x, x);
				}
				row >>= 1;
				x++;
			}
		}
		if (max_y < 0) {
			return Rect2i();
		}
		return Rect2i(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
	}

	bool covers(const Rect2i &p_tiles) const {
		if (p_tiles.size.x <= 0 || p_tiles.size.y <= 0) {
			return true;
		}
		const uint64_t bits = row_bits(p_tiles.position.x, p_tiles.size.x);
		for (int y = p_tiles.position.y; y < p_tiles.position.y + p_tiles.size.y; y++) {
			if ((rows[y] & bits) != bits) {
				return false;
			}
		}
		return true;
	}

	// Охватывающий прямоугольник отмеченных плиток внутри заданного; пустой, если таких нет.
	Rect2i bounds_within(const Rect2i &p_tiles) const {
		if (p_tiles.size.x <= 0 || p_tiles.size.y <= 0) {
			return Rect2i();
		}
		const uint64_t bits = row_bits(p_tiles.position.x, p_tiles.size.x);
		int min_x = SIZE, max_x = -1, min_y = SIZE, max_y = -1;
		for (int y = p_tiles.position.y; y < p_tiles.position.y + p_tiles.size.y; y++) {
			uint64_t row = rows[y] & bits;
			if (row == 0) {
				continue;
			}
			min_y = MIN(min_y, y);
			max_y = y;
			int x = p_tiles.position.x;
			row >>= x;
			while (row != 0) {
				if (row & 1) {
					min_x = MIN(min_x, x);
					max_x = MAX(max_x, x);
				}
				row >>= 1;
				x++;
			}
		}
		if (max_y < 0) {
			return Rect2i();
		}
		return Rect2i(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
	}

	uint64_t get_row(int p_row) const { return rows[p_row]; }
};
