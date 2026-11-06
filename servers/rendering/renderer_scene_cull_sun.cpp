/**************************************************************************/
/*  renderer_scene_cull_sun.cpp                                           */
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

// lestoroer#sun: Карта солнца по кастерам: область и окна, постоянная карта, шаги её обновления и отладка.

#include "renderer_scene_cull.h"

#include "core/math/geometry_3d.h"
#include "servers/rendering/renderer_compositor.h"

// lestoroer#sun
static bool _sun_is_geometry(const RendererSceneCull::InstanceData &p_data) {
	return ((1 << (p_data.flags & RendererSceneCull::InstanceData::FLAG_BASE_TYPE_MASK)) & RSE::INSTANCE_GEOMETRY_MASK) != 0;
}

static bool _sun_is_geometry(const RendererSceneCull::Instance *p_instance) {
	return ((1 << p_instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) != 0;
}

// lestoroer#sun: Единственная точка, через которую карта солнца узнаёт об изменении геометрии.
void RendererSceneCull::_sun_caster_changed(Instance *p_instance, uint32_t p_change) const {
	Scenario *scenario = p_instance->scenario;
	if (scenario == nullptr || !_sun_is_geometry(p_instance) || (sun_map_debug.ignored_changes & p_change)) {
		return;
	}
	scenario->sun_casters_version++;
	if (scenario->sun_map.light_instance.is_valid() && !p_instance->sun_changed_item.in_list()) {
		scenario->sun_map.changed.add(&p_instance->sun_changed_item);
	}
}

// lestoroer#sun
bool RendererSceneCull::_sun_is_caster(const InstanceData &p_data, uint32_t p_caster_mask) {
	return _sun_is_geometry(p_data) && (p_data.flags & InstanceData::FLAG_CAST_SHADOWS) && (p_data.layer_mask & p_caster_mask) && static_cast<InstanceGeometryData *>(p_data.instance->base_data)->can_cast_shadows;
}

// lestoroer#sun: Границы кастера центром и полуразмером.
struct SunBox {
	Vector3 center;
	Vector3 extents;
};

static SunBox _sun_box_of(const RendererSceneCull::InstanceBounds &p_bounds) {
	const real_t *bounds = p_bounds.bounds;
	return { Vector3(bounds[0] + bounds[3], bounds[1] + bounds[4], bounds[2] + bounds[5]) * 0.5, Vector3(bounds[3] - bounds[0], bounds[4] - bounds[1], bounds[5] - bounds[2]) * 0.5 };
}

static SunBox _sun_box_of(const AABB &p_aabb) {
	return { p_aabb.get_center(), p_aabb.size * 0.5 };
}

// lestoroer#sun: Прямоугольник в плоскости карты.
struct SunPlaneRect {
	Vector2 min;
	Vector2 max;
};

// lestoroer#sun: Оси плоскости карты и проекция кастера на неё.
struct SunPlaneAxes {
	Vector3 x;
	Vector3 y;
	Vector3 x_abs;
	Vector3 y_abs;

	SunPlaneAxes(const Vector3 &p_x, const Vector3 &p_y) :
			x(p_x), y(p_y), x_abs(p_x.abs()), y_abs(p_y.abs()) {}

	Vector2 center_of(const SunBox &p_box) const {
		return Vector2(x.dot(p_box.center), y.dot(p_box.center));
	}

	SunPlaneRect rect_of(const SunBox &p_box) const {
		const Vector2 center = center_of(p_box);
		const Vector2 extent(x_abs.dot(p_box.extents), y_abs.dot(p_box.extents));
		return { center - extent, center + extent };
	}
};

// lestoroer#sun: Глубина кастера вдоль света.
struct SunDepthRange {
	real_t low = 0.0;
	real_t high = 0.0;
};

static SunDepthRange _sun_depth_of(const Vector3 &p_z, const Vector3 &p_z_abs, const SunBox &p_box) {
	const real_t center = p_z.dot(p_box.center);
	const real_t extent = p_z_abs.dot(p_box.extents);
	return { center - extent, center + extent };
}

// lestoroer#sun: Предмет, сдвинутый в пределах клетки, не меняет карту.
static constexpr real_t SUN_PLANE_GRID_METERS = 0.1;

// lestoroer#sun: Прямоугольник, расширенный до клеток сетки плоскости.
static SunPlaneRect _sun_grid_rect(const Vector2 &p_min, const Vector2 &p_max) {
	return { Vector2(Math::floor(p_min.x / SUN_PLANE_GRID_METERS), Math::floor(p_min.y / SUN_PLANE_GRID_METERS)) * SUN_PLANE_GRID_METERS,
		Vector2(Math::ceil(p_max.x / SUN_PLANE_GRID_METERS), Math::ceil(p_max.y / SUN_PLANE_GRID_METERS)) * SUN_PLANE_GRID_METERS };
}

// lestoroer#sun: Границы всей геометрии мира. Тем же проходом каждый кастер передаётся в p_visit_caster.
template <typename VisitCaster>
static AABB _sun_world_bounds(const RendererSceneCull::Scenario *p_scenario, uint32_t p_caster_mask, const VisitCaster &p_visit_caster) {
	Vector3 world_min(Math::INF, Math::INF, Math::INF);
	Vector3 world_max(-Math::INF, -Math::INF, -Math::INF);
	for (uint64_t i = 0; i < p_scenario->instance_data.size(); i++) {
		const RendererSceneCull::InstanceData &idata = p_scenario->instance_data[i];
		if (!_sun_is_geometry(idata)) {
			continue;
		}
		const real_t *bounds = p_scenario->instance_aabbs[i].bounds;
		world_min = world_min.min(Vector3(bounds[0], bounds[1], bounds[2]));
		world_max = world_max.max(Vector3(bounds[3], bounds[4], bounds[5]));
		if (RendererSceneCull::_sun_is_caster(idata, p_caster_mask)) {
			p_visit_caster(_sun_box_of(p_scenario->instance_aabbs[i]), static_cast<const RendererSceneCull::InstanceGeometryData *>(idata.instance->base_data));
		}
	}
	return world_min.x == Math::INF ? AABB() : AABB(world_min, world_max - world_min);
}

// lestoroer#sun: Самый плотный поворот карты; текущий остаётся, если выигрыш меньше ROLL_SWITCH_GAIN.
template <typename TexelAtRoll>
static int _sun_best_roll(int p_current_roll, const TexelAtRoll &p_texel_at_roll) {
	int best_roll = 0;
	real_t best_texel = Math::INF;
	real_t current_texel = Math::INF;
	for (int roll = 0; roll < RendererSceneCull::SunCasterBounds::ROLL_CANDIDATES; roll++) {
		const real_t texel = p_texel_at_roll(roll);
		if (texel < best_texel) {
			best_texel = texel;
			best_roll = roll;
		}
		if (roll == p_current_roll) {
			current_texel = texel;
		}
	}
	if (p_current_roll >= 0 && !(best_texel < current_texel * RendererSceneCull::SunCasterBounds::ROLL_SWITCH_GAIN)) {
		return p_current_roll;
	}
	return best_roll;
}

// lestoroer#sun: Поля окна в атласе в долях margin_texels. Единственное окно читает вся сцена, поэтому ему хватает поля на фильтр.
// При нескольких окнах точка читает окно только внутри одного поля, а кастер лежит ещё на поле глубже: хвост его тени попадает в окно.
static constexpr real_t SUN_RING_MARGINS_ONE_WINDOW = 1.0;
static constexpr real_t SUN_RING_MARGINS_MANY_WINDOWS = 2.0;
// lestoroer#sun: Запас окна-попутчика: предмет идёт внутри него, не двигая окно каждый кадр.
static constexpr real_t SUN_COMPANION_SLACK_FRACTION = 0.25;

// lestoroer#sun: Варианты размещения группы окон в атласе: каждый разрез делит группу на две, рядом или друг над другом.
struct SunPackOption {
	Vector2i size;
	uint8_t first_mask = 0;
	uint8_t first_option = 0;
	uint8_t second_option = 0;
	bool side_by_side = false;
};

static constexpr int SUN_PACK_MAX_OPTIONS = 12;
static constexpr int SUN_PACK_MASKS = 1 << RendererSceneCull::SUN_MAX_WINDOWS;

// lestoroer#sun: Оставляет только размеры, не хуже которых нет: вариант, больший другого по обеим сторонам, бесполезен.
static void _sun_pack_add_option(SunPackOption *r_options, int &r_count, const SunPackOption &p_option) {
	if (p_option.size.x > SunTileMask::SIZE || p_option.size.y > SunTileMask::SIZE) {
		return;
	}
	for (int i = 0; i < r_count; i++) {
		if (r_options[i].size.x <= p_option.size.x && r_options[i].size.y <= p_option.size.y) {
			return;
		}
	}
	int kept = 0;
	for (int i = 0; i < r_count; i++) {
		if (!(p_option.size.x <= r_options[i].size.x && p_option.size.y <= r_options[i].size.y)) {
			r_options[kept++] = r_options[i];
		}
	}
	if (kept < SUN_PACK_MAX_OPTIONS) {
		r_options[kept++] = p_option;
	}
	r_count = kept;
}

// lestoroer#sun
static int _sun_pack_lowest_bit(int p_mask) {
	int index = 0;
	while (!(p_mask & (1 << index))) {
		index++;
	}
	return index;
}

// lestoroer#sun
static void _sun_pack_place(const SunPackOption p_options[][SUN_PACK_MAX_OPTIONS], int p_mask, int p_option, const Vector2i &p_position, Rect2i *r_tiles) {
	const SunPackOption &option = p_options[p_mask][p_option];
	if ((p_mask & (p_mask - 1)) == 0) {
		r_tiles[_sun_pack_lowest_bit(p_mask)] = Rect2i(p_position, option.size);
		return;
	}
	const int second_mask = p_mask ^ option.first_mask;
	const Vector2i first_size = p_options[option.first_mask][option.first_option].size;
	_sun_pack_place(p_options, option.first_mask, option.first_option, p_position, r_tiles);
	_sun_pack_place(p_options, second_mask, option.second_option, p_position + (option.side_by_side ? Vector2i(first_size.x, 0) : Vector2i(0, first_size.y)), r_tiles);
}

// lestoroer#sun: Раскладывает прямоугольники размером в плитках по атласу. false, если они не помещаются.
static bool _sun_pack_tiles(const Vector2i *p_sizes, int p_count, Rect2i *r_tiles) {
	SunPackOption options[SUN_PACK_MASKS][SUN_PACK_MAX_OPTIONS];
	int option_count[SUN_PACK_MASKS] = {};
	const int full_mask = (1 << p_count) - 1;
	for (int mask = 1; mask <= full_mask; mask++) {
		if ((mask & (mask - 1)) == 0) {
			SunPackOption option;
			option.size = p_sizes[_sun_pack_lowest_bit(mask)];
			_sun_pack_add_option(options[mask], option_count[mask], option);
			continue;
		}
		for (int first = (mask - 1) & mask; first > 0; first = (first - 1) & mask) {
			const int second = mask ^ first;
			if (first < second) {
				continue;
			}
			for (int i = 0; i < option_count[first]; i++) {
				for (int j = 0; j < option_count[second]; j++) {
					const Vector2i a = options[first][i].size;
					const Vector2i b = options[second][j].size;
					SunPackOption option;
					option.first_mask = first;
					option.first_option = i;
					option.second_option = j;
					option.side_by_side = true;
					option.size = Vector2i(a.x + b.x, MAX(a.y, b.y));
					_sun_pack_add_option(options[mask], option_count[mask], option);
					option.side_by_side = false;
					option.size = Vector2i(MAX(a.x, b.x), a.y + b.y);
					_sun_pack_add_option(options[mask], option_count[mask], option);
				}
			}
		}
	}
	if (option_count[full_mask] == 0) {
		return false;
	}
	_sun_pack_place(options, full_mask, 0, Vector2i(), r_tiles);
	return true;
}

// lestoroer#sun: Наименьший тексель, общий для всех окон, при котором их прямоугольники помещаются в атлас.
// Один размер текселя на всю карту: предмет, перешедший из окна в окно, не меняет резкость тени.
static real_t _sun_pack_windows(const Vector2 *p_plane_sizes, int p_count, real_t p_margin_texels, int p_map_size, Rect2i *r_tiles) {
	static constexpr int SEARCH_STEPS = 20;
	const real_t tile = p_map_size / SunTileMask::SIZE;
	// Плюс тексель на сдвиг центра окна к сетке текселей.
	const real_t ring = (p_margin_texels * SUN_RING_MARGINS_MANY_WINDOWS + 1.0) * 2.0;

	const auto tiles_at = [&](real_t p_texel, Vector2i *r_sizes) {
		for (int i = 0; i < p_count; i++) {
			r_sizes[i] = Vector2i(Math::ceil((p_plane_sizes[i].x / p_texel + ring) / tile), Math::ceil((p_plane_sizes[i].y / p_texel + ring) / tile));
		}
	};

	real_t total_area = 0.0;
	real_t total_width = 0.0;
	real_t tallest = 0.0;
	for (int i = 0; i < p_count; i++) {
		total_area += p_plane_sizes[i].x * p_plane_sizes[i].y;
		total_width += p_plane_sizes[i].x;
		tallest = MAX(tallest, p_plane_sizes[i].y);
	}
	real_t low = Math::sqrt(total_area) / p_map_size;
	// Все окна в один ряд помещаются заведомо, если оставить место на поля и округление до плиток.
	real_t high = MAX(total_width, tallest) / MAX(p_map_size - p_count * (ring + tile), tile);

	Vector2i sizes[RendererSceneCull::SUN_MAX_WINDOWS];
	for (int step = 0; step < SEARCH_STEPS; step++) {
		const real_t middle = (low + high) * 0.5;
		tiles_at(middle, sizes);
		if (_sun_pack_tiles(sizes, p_count, r_tiles)) {
			high = middle;
		} else {
			low = middle;
		}
	}
	tiles_at(high, sizes);
	while (!_sun_pack_tiles(sizes, p_count, r_tiles)) {
		high *= 1.05;
		tiles_at(high, sizes);
	}
	return high;
}

// lestoroer#sun: Тексель при таких размерах групп в плоскости солнца.
static real_t _sun_windows_texel(const Vector2 *p_plane_sizes, int p_count, real_t p_margin_texels, int p_map_size) {
	if (p_count == 1) {
		return MAX(MAX(p_plane_sizes[0].x, p_plane_sizes[0].y), RendererSceneCull::SUN_MIN_WINDOW_METERS) / (p_map_size - p_margin_texels * SUN_RING_MARGINS_ONE_WINDOW * 2.0);
	}
	Vector2 sizes[RendererSceneCull::SUN_MAX_WINDOWS];
	Rect2i tiles[RendererSceneCull::SUN_MAX_WINDOWS];
	for (int i = 0; i < p_count; i++) {
		sizes[i] = p_plane_sizes[i].maxf(RendererSceneCull::SUN_MIN_WINDOW_METERS);
	}
	return _sun_pack_windows(sizes, p_count, p_margin_texels, p_map_size, tiles);
}

// lestoroer#sun: Свободные плитки вокруг окна становятся запасом для движения: предмету не нужно менять раскладку.
static void _sun_spread_into_free_tiles(Rect2i *r_tiles, int p_count) {
	static constexpr real_t MAX_SPREAD = 0.15;
	SunTileMask used;
	Vector2i limit[RendererSceneCull::SUN_MAX_WINDOWS];
	for (int i = 0; i < p_count; i++) {
		used.set_rect(r_tiles[i]);
		limit[i] = r_tiles[i].size + Vector2i(Math::ceil(r_tiles[i].size.x * MAX_SPREAD), Math::ceil(r_tiles[i].size.y * MAX_SPREAD));
	}
	bool grew = true;
	while (grew) {
		grew = false;
		for (int i = 0; i < p_count; i++) {
			const Rect2i &rect = r_tiles[i];
			const Rect2i right(rect.position.x + rect.size.x, rect.position.y, 1, rect.size.y);
			if (rect.size.x < limit[i].x && right.position.x < SunTileMask::SIZE && !used.intersects(right)) {
				used.set_rect(right);
				r_tiles[i].size.x++;
				grew = true;
			}
			const Rect2i below(rect.position.x, rect.position.y + rect.size.y, rect.size.x, 1);
			if (rect.size.y < limit[i].y && below.position.y < SunTileMask::SIZE && !used.intersects(below)) {
				used.set_rect(below);
				r_tiles[i].size.y++;
				grew = true;
			}
		}
	}
}

// lestoroer#sun: Группа кастеров при подгонке, в плоскости солнца.
struct SunFitGroup {
	Vector2 min;
	Vector2 max;
	LocalVector<uint32_t> members;
};

struct SunFitGroupLarger {
	_FORCE_INLINE_ bool operator()(const SunFitGroup &p_a, const SunFitGroup &p_b) const {
		const Vector2 size_a = p_a.max - p_a.min;
		const Vector2 size_b = p_b.max - p_b.min;
		return size_a.x * size_a.y > size_b.x * size_b.y;
	}
};

// lestoroer#sun
static real_t _sun_fit_groups_texel(const LocalVector<SunFitGroup> &p_groups, real_t p_margin_texels, int p_map_size) {
	Vector2 sizes[RendererSceneCull::SUN_MAX_WINDOWS];
	for (uint32_t i = 0; i < p_groups.size(); i++) {
		sizes[i] = p_groups[i].max - p_groups[i].min;
	}
	return _sun_windows_texel(sizes, p_groups.size(), p_margin_texels, p_map_size);
}

// lestoroer#sun: Разрез группы кастеров по зазору, в который не заходит ни один её кастер.
struct SunFitCut {
	uint32_t group = 0;
	int axis = 0;
	real_t position = 0.0;
	real_t width = 0.0;

	bool operator<(const SunFitCut &p_other) const {
		return width > p_other.width;
	}
};

struct SunFitInterval {
	real_t from = 0.0;
	real_t to = 0.0;

	bool operator<(const SunFitInterval &p_other) const {
		return from < p_other.from;
	}
};

// lestoroer#sun
static void _sun_fit_add_widest_cut(LocalVector<SunFitCut> &r_cuts, uint32_t p_group, int p_axis, const LocalVector<SunFitGroup> &p_groups, const LocalVector<Vector2> &p_rect_min, const LocalVector<Vector2> &p_rect_max) {
	// Зазор, меньше которого отдельное окно ничего не даёт: у каждого окна свои поля.
	static constexpr real_t MIN_GAP_METERS = 0.1;

	const LocalVector<uint32_t> &members = p_groups[p_group].members;
	LocalVector<SunFitInterval> intervals;
	intervals.resize(members.size());
	for (uint32_t i = 0; i < members.size(); i++) {
		intervals[i].from = p_rect_min[members[i]][p_axis];
		intervals[i].to = p_rect_max[members[i]][p_axis];
	}
	intervals.sort();

	SunFitCut cut;
	cut.group = p_group;
	cut.axis = p_axis;
	real_t reached = intervals[0].to;
	for (uint32_t i = 1; i < intervals.size(); i++) {
		const real_t gap = intervals[i].from - reached;
		if (gap > cut.width) {
			cut.width = gap;
			cut.position = reached + gap * 0.5;
		}
		reached = MAX(reached, intervals[i].to);
	}
	if (cut.width >= MIN_GAP_METERS) {
		r_cuts.push_back(cut);
	}
}

// lestoroer#sun
static void _sun_fit_apply_cut(LocalVector<SunFitGroup> &r_groups, const SunFitCut &p_cut, const LocalVector<Vector2> &p_rect_min, const LocalVector<Vector2> &p_rect_max) {
	const SunFitGroup whole = r_groups[p_cut.group];
	SunFitGroup first;
	SunFitGroup second;
	first.min = second.min = Vector2(Math::INF, Math::INF);
	first.max = second.max = Vector2(-Math::INF, -Math::INF);
	for (const uint32_t member : whole.members) {
		SunFitGroup &target = p_rect_max[member][p_cut.axis] <= p_cut.position ? first : second;
		target.members.push_back(member);
		target.min = target.min.min(p_rect_min[member]);
		target.max = target.max.max(p_rect_max[member]);
	}
	r_groups[p_cut.group] = first;
	r_groups.push_back(second);
}

// lestoroer#sun: Делит кастеры на группы по зазорам в плоскости солнца: разнесённые постройки не затеняют друг друга.
// Разрез принимается, только если заметно улучшает тексель: каждое окно платит полями и округлением до плиток.
static void _sun_fit_cut_groups(LocalVector<SunFitGroup> &r_groups, const LocalVector<Vector2> &p_rect_min, const LocalVector<Vector2> &p_rect_max, int p_max_groups, real_t p_margin_texels, int p_map_size) {
	static constexpr real_t CUT_GAIN = 0.98;

	r_groups.clear();
	r_groups.resize(1);
	SunFitGroup &all = r_groups[0];
	all.min = Vector2(Math::INF, Math::INF);
	all.max = Vector2(-Math::INF, -Math::INF);
	all.members.resize(p_rect_min.size());
	for (uint32_t i = 0; i < p_rect_min.size(); i++) {
		all.members[i] = i;
		all.min = all.min.min(p_rect_min[i]);
		all.max = all.max.max(p_rect_max[i]);
	}

	real_t texel = _sun_fit_groups_texel(r_groups, p_margin_texels, p_map_size);
	while (int(r_groups.size()) < p_max_groups) {
		LocalVector<SunFitCut> cuts;
		for (uint32_t group = 0; group < r_groups.size(); group++) {
			_sun_fit_add_widest_cut(cuts, group, 0, r_groups, p_rect_min, p_rect_max);
			_sun_fit_add_widest_cut(cuts, group, 1, r_groups, p_rect_min, p_rect_max);
		}
		cuts.sort();
		bool accepted = false;
		for (const SunFitCut &cut : cuts) {
			LocalVector<SunFitGroup> trial(r_groups);
			_sun_fit_apply_cut(trial, cut, p_rect_min, p_rect_max);
			const real_t trial_texel = _sun_fit_groups_texel(trial, p_margin_texels, p_map_size);
			if (trial_texel < texel * CUT_GAIN) {
				r_groups = trial;
				texel = trial_texel;
				accepted = true;
				break;
			}
		}
		if (!accepted) {
			break;
		}
	}
}

// lestoroer#sun: Поля карты из настроек света: фильтр в текселях и смещение по нормали.
real_t RendererSceneCull::_sun_map_margin_texels(const Instance *p_light) {
	// Наибольший множитель качества фильтра солнца: движок домножает на него размытие в текселях.
	static constexpr real_t MAX_FILTER_QUALITY_RADIUS = 4.0;
	const real_t blur_texels = RSG::light_storage->light_get_param(p_light->base, RSE::LIGHT_PARAM_SHADOW_BLUR) * MAX_FILTER_QUALITY_RADIUS;
	const real_t normal_bias_texels = RSG::light_storage->light_get_param(p_light->base, RSE::LIGHT_PARAM_SHADOW_NORMAL_BIAS);
	return Math::ceil(blur_texels + normal_bias_texels) + 2.0;
}

// lestoroer#sun: Глубина прямоугольника плоскости солнца: кастеры, которых он касается. Шейдер окон поднимает точку ниже дальней плоскости к ней,
// поэтому получатели глубину не расширяют. Иначе точка ниже дальней плоскости попала бы в тень, и дальняя плоскость накрывает всю геометрию.
void RendererSceneCull::_sun_group_fit_depth(SunGroup &r_group, const SunCasterBounds &p_bounds, const Vector2 &p_rect_min, const Vector2 &p_rect_max, const Scenario *p_scenario) {
	const Vector3 light_z = p_bounds.light_basis.get_column(2);
	const Vector3 light_z_abs = light_z.abs();
	const SunPlaneAxes axes(p_bounds.map_x, p_bounds.map_y);
	real_t caster_z_max = -Math::INF;
	real_t z_min = Math::INF;
	for (uint64_t i = 0; i < p_scenario->instance_data.size(); i++) {
		const InstanceData &idata = p_scenario->instance_data[i];
		if (!_sun_is_geometry(idata)) {
			continue;
		}
		const bool caster = _sun_is_caster(idata, p_bounds.settings.caster_mask);
		if (!caster && !p_bounds.settings.covers_receivers) {
			continue;
		}
		const SunBox box = _sun_box_of(p_scenario->instance_aabbs[i]);
		if (!p_bounds.settings.covers_receivers) {
			const SunPlaneRect rect = axes.rect_of(box);
			if (rect.max.x < p_rect_min.x || rect.min.x > p_rect_max.x || rect.max.y < p_rect_min.y || rect.min.y > p_rect_max.y) {
				continue;
			}
		}
		const SunDepthRange depth = _sun_depth_of(light_z, light_z_abs, box);
		z_min = MIN(z_min, depth.low);
		if (caster) {
			caster_z_max = MAX(caster_z_max, depth.high);
		}
	}
	if (z_min == Math::INF) {
		r_group.z_max = 0.0;
		r_group.z_min = 0.0;
		return;
	}
	r_group.z_min = z_min;
	r_group.z_max = caster_z_max == -Math::INF ? z_min : caster_z_max;
}

// lestoroer#sun: Плотные группы всех кастеров в осях карты, без учёта текущего состояния области.
void RendererSceneCull::_sun_caster_bounds_fit(SunCasterBounds &r_fit, const Scenario *p_scenario, const Basis &p_light_basis, const SunFitSettings &p_settings, int p_current_roll, bool p_all_rolls) const {
	LocalVector<SunBox> casters;
	r_fit.world_bounds = _sun_world_bounds(p_scenario, p_settings.caster_mask, [&](const SunBox &p_box, const InstanceGeometryData *) {
		casters.push_back(p_box);
	});
	r_fit.casters_version = p_scenario->sun_casters_version;
	r_fit.fit_world = p_scenario->self;
	r_fit.light_basis = p_light_basis;
	r_fit.settings = p_settings;

	if (casters.is_empty()) {
		// Кастеров нет: карта пуста, важна только глубина получателей.
		r_fit.roll_index = 0;
		r_fit.map_x = p_light_basis.get_column(0);
		r_fit.map_y = p_light_basis.get_column(1);
		r_fit.group_count = 1;
		r_fit.groups[0] = SunGroup();
		_sun_group_fit_depth(r_fit.groups[0], r_fit, Vector2(), Vector2(), p_scenario);
		return;
	}

	LocalVector<Vector2> rect_min;
	LocalVector<Vector2> rect_max;
	rect_min.resize(casters.size());
	rect_max.resize(casters.size());
	LocalVector<SunFitGroup> groups;

	const auto texel_at_roll = [&](int p_roll) {
		Vector3 x;
		Vector3 y;
		_sun_roll_axes(p_light_basis, p_roll, x, y);
		const SunPlaneAxes axes(x, y);
		for (uint32_t i = 0; i < casters.size(); i++) {
			const SunPlaneRect rect = axes.rect_of(casters[i]);
			rect_min[i] = rect.min;
			rect_max[i] = rect.max;
		}
		_sun_fit_cut_groups(groups, rect_min, rect_max, p_settings.max_groups, p_settings.margin_texels, p_settings.map_size);
		return _sun_fit_groups_texel(groups, p_settings.margin_texels, p_settings.map_size);
	};

	const int roll = p_all_rolls || p_current_roll < 0 ? _sun_best_roll(p_current_roll, texel_at_roll) : p_current_roll;
	texel_at_roll(roll);
	_sun_roll_axes(p_light_basis, roll, r_fit.map_x, r_fit.map_y);
	r_fit.roll_index = roll;

	// Сначала самые крупные: большинство точек экрана находит своё окно с первой проверки.
	groups.sort_custom<SunFitGroupLarger>();

	r_fit.group_count = groups.size();
	for (int i = 0; i < r_fit.group_count; i++) {
		SunGroup &group = r_fit.groups[i];
		group = SunGroup();
		const SunPlaneRect rect = _sun_grid_rect(groups[i].min, groups[i].max);
		group.plane_min = rect.min;
		group.plane_max = rect.max;
		_sun_group_fit_depth(group, r_fit, group.plane_min, group.plane_max, p_scenario);
	}
}

// lestoroer#sun
void RendererSceneCull::_sun_roll_axes(const Basis &p_light_basis, int p_roll, Vector3 &r_x, Vector3 &r_y) {
	const real_t angle = Math::PI * 0.5 * p_roll / SunCasterBounds::ROLL_CANDIDATES;
	const real_t cosine = Math::cos(angle);
	const real_t sine = Math::sin(angle);
	r_x = p_light_basis.get_column(0) * cosine + p_light_basis.get_column(1) * sine;
	r_y = p_light_basis.get_column(1) * cosine - p_light_basis.get_column(0) * sine;
}

// lestoroer#sun: Запоминает у каждого кастера его группу: при движении регулятора солнца состав групп не меняется.
void RendererSceneCull::_sun_assign_caster_groups(const SunCasterBounds &p_bounds, const Scenario *p_scenario) {
	const SunPlaneAxes axes(p_bounds.map_x, p_bounds.map_y);
	for (uint64_t i = 0; i < p_scenario->instance_data.size(); i++) {
		const InstanceData &idata = p_scenario->instance_data[i];
		if (!_sun_is_caster(idata, p_bounds.settings.caster_mask)) {
			continue;
		}
		const SunPlaneRect rect = axes.rect_of(_sun_box_of(p_scenario->instance_aabbs[i]));
		int member_of = 0;
		for (int group = 0; group < p_bounds.group_count; group++) {
			const SunGroup &candidate = p_bounds.groups[group];
			if (rect.min.x >= candidate.plane_min.x && rect.min.y >= candidate.plane_min.y && rect.max.x <= candidate.plane_max.x && rect.max.y <= candidate.plane_max.y) {
				member_of = group;
				break;
			}
		}
		static_cast<InstanceGeometryData *>(idata.instance->base_data)->sun_group = member_of;
	}
}

// lestoroer#sun: Регулятор солнца движется: состав групп не меняется, а поворот карты выбирается каждый кадр с тем же запасом, что и в покое.
// Поэтому в покое плотные группы совпадают с теми, что были в движении, и новой раскладки после остановки нет.
// Окна могут перекрыться, кастеры в перекрытии рисуются в обоих.
void RendererSceneCull::_sun_caster_bounds_follow_direction(SunCasterBounds &r_bounds, const Scenario *p_scenario, const Basis &p_light_basis) const {
	r_bounds.light_basis = p_light_basis;

	LocalVector<SunBox> casters;
	LocalVector<int> caster_groups;
	bool group_used[SUN_MAX_WINDOWS] = {};
	r_bounds.world_bounds = _sun_world_bounds(p_scenario, r_bounds.settings.caster_mask, [&](const SunBox &p_box, const InstanceGeometryData *p_geom) {
		const int group = MIN(int(p_geom->sun_group), r_bounds.group_count - 1);
		casters.push_back(p_box);
		caster_groups.push_back(group);
		group_used[group] = true;
	});

	Vector2 group_min[SUN_MAX_WINDOWS];
	Vector2 group_max[SUN_MAX_WINDOWS];
	const auto texel_at_roll = [&](int p_roll) {
		Vector3 x;
		Vector3 y;
		_sun_roll_axes(p_light_basis, p_roll, x, y);
		const SunPlaneAxes axes(x, y);
		for (int group = 0; group < r_bounds.group_count; group++) {
			group_min[group] = Vector2(Math::INF, Math::INF);
			group_max[group] = Vector2(-Math::INF, -Math::INF);
		}
		for (uint32_t i = 0; i < casters.size(); i++) {
			const SunPlaneRect rect = axes.rect_of(casters[i]);
			group_min[caster_groups[i]] = group_min[caster_groups[i]].min(rect.min);
			group_max[caster_groups[i]] = group_max[caster_groups[i]].max(rect.max);
		}
		Vector2 sizes[SUN_MAX_WINDOWS];
		int used = 0;
		for (int group = 0; group < r_bounds.group_count; group++) {
			if (group_used[group]) {
				sizes[used++] = group_max[group] - group_min[group];
			}
		}
		return _sun_windows_texel(sizes, used, r_bounds.settings.margin_texels, r_bounds.settings.map_size);
	};

	if (!casters.is_empty()) {
		r_bounds.roll_index = _sun_best_roll(r_bounds.roll_index, texel_at_roll);
		texel_at_roll(r_bounds.roll_index);
	}
	_sun_roll_axes(p_light_basis, r_bounds.roll_index, r_bounds.map_x, r_bounds.map_y);

	int new_index[SUN_MAX_WINDOWS];
	int kept = 0;
	for (int group = 0; group < r_bounds.group_count; group++) {
		new_index[group] = kept;
		if (!group_used[group]) {
			continue;
		}
		SunGroup &target = r_bounds.groups[kept];
		target.travelling = r_bounds.groups[group].travelling;
		const SunPlaneRect rect = _sun_grid_rect(group_min[group], group_max[group]);
		target.plane_min = rect.min;
		target.plane_max = rect.max;
		kept++;
	}
	if (kept == 0) {
		r_bounds.groups[0] = SunGroup();
		kept = 1;
	}
	if (kept != r_bounds.group_count) {
		for (uint64_t i = 0; i < p_scenario->instance_data.size(); i++) {
			const InstanceData &idata = p_scenario->instance_data[i];
			if (_sun_is_geometry(idata)) {
				InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(idata.instance->base_data);
				geom->sun_group = MIN(new_index[MIN(int(geom->sun_group), r_bounds.group_count - 1)], kept - 1);
			}
		}
		r_bounds.group_count = kept;
	}
	if (r_bounds.group_count == 1) {
		_sun_group_fit_depth(r_bounds.groups[0], r_bounds, r_bounds.groups[0].plane_min, r_bounds.groups[0].plane_max, p_scenario);
	}
}

// lestoroer#sun: Окна карты из групп. Раскладка атласа меняется, только когда p_pack истинно или группа перестала помещаться в своё окно.
// Настройки света читаются здесь, поэтому их смена не требует отдельного учёта.
void RendererSceneCull::_sun_map_update_frame(SunMapFrame &r_frame, SunCasterBounds &r_bounds, const Instance *p_light, bool p_pack, const Scenario *p_scenario) const {
	// Запас глубины на смещение вершин материалом и неточность границ.
	static constexpr real_t DEPTH_MARGIN_METERS = 0.1;

	const SunFitSettings &settings = r_bounds.settings;
	const Vector3 z_axis = r_bounds.light_basis.get_column(2);
	const int count = r_bounds.group_count;
	const bool same_map = r_frame.window_count == count && r_frame.size == settings.map_size && r_frame.margin_texels == settings.margin_texels && r_frame.x == r_bounds.map_x && r_frame.y == r_bounds.map_y && r_frame.z == z_axis;

	r_frame.x = r_bounds.map_x;
	r_frame.y = r_bounds.map_y;
	r_frame.z = z_axis;
	r_frame.size = settings.map_size;
	r_frame.tile_size = settings.map_size / SunTileMask::SIZE;
	r_frame.margin_texels = settings.margin_texels;
	r_frame.reverse_cull = RSG::light_storage->light_get_reverse_cull_face_mode(p_light->base);
	r_frame.window_count = count;

	if (count == 1) {
		const SunGroup &group = r_bounds.groups[0];
		SunMapWindow &window = r_frame.windows[0];
		window.z_max = group.z_max + DEPTH_MARGIN_METERS;
		window.z_min = group.z_min - DEPTH_MARGIN_METERS;
		const real_t light_angle = RSG::light_storage->light_get_param(p_light->base, RSE::LIGHT_PARAM_SIZE);
		const real_t penumbra_expand = light_angle > 0.0 ? Math::tan(Math::deg_to_rad(light_angle)) * (window.z_max - window.z_min) : 0.0;
		const Vector2 plane_size = group.plane_max - group.plane_min;
		const real_t covered_side = MAX(MAX(plane_size.x, plane_size.y), SUN_MIN_WINDOW_METERS) + penumbra_expand * 2.0;
		r_frame.texel_size = covered_side / (settings.map_size - settings.margin_texels * SUN_RING_MARGINS_ONE_WINDOW * 2.0);
		const real_t half_side = r_frame.texel_size * settings.map_size * 0.5;
		window.half_size = Vector2(half_side, half_side);
		// Центр на сетке текселей: при сдвиге области без смены размера края теней не плывут.
		window.center = ((group.plane_min + group.plane_max) * 0.5).snappedf(r_frame.texel_size);
		window.tiles = Rect2i(0, 0, SunTileMask::SIZE, SunTileMask::SIZE);
		return;
	}

	Vector2 plane_sizes[SUN_MAX_WINDOWS];
	for (int i = 0; i < count; i++) {
		plane_sizes[i] = (r_bounds.groups[i].plane_max - r_bounds.groups[i].plane_min).maxf(SUN_MIN_WINDOW_METERS);
	}

	bool keep_layout = same_map && !p_pack;
	if (keep_layout) {
		const real_t ring = (settings.margin_texels * SUN_RING_MARGINS_MANY_WINDOWS + 1.0) * 2.0;
		for (int i = 0; i < count && keep_layout; i++) {
			const Vector2 capacity = (Vector2(_sun_map_texels_of(r_frame, r_frame.windows[i].tiles).size) - Vector2(ring, ring)) * r_frame.texel_size;
			keep_layout = plane_sizes[i].x <= capacity.x && plane_sizes[i].y <= capacity.y;
		}
	}
	if (!keep_layout) {
		Rect2i tiles[SUN_MAX_WINDOWS];
		r_frame.texel_size = _sun_pack_windows(plane_sizes, count, settings.margin_texels, settings.map_size, tiles);
		_sun_spread_into_free_tiles(tiles, count);
		for (int i = 0; i < count; i++) {
			r_frame.windows[i].tiles = tiles[i];
		}
	}
	for (int i = 0; i < count; i++) {
		SunGroup &group = r_bounds.groups[i];
		SunMapWindow &window = r_frame.windows[i];
		const Vector2 half_size = Vector2(_sun_map_texels_of(r_frame, window.tiles).size) * r_frame.texel_size * 0.5;
		const Vector2 center = ((group.plane_min + group.plane_max) * 0.5).snappedf(r_frame.texel_size);
		if (!keep_layout || center != window.center || half_size != window.half_size) {
			window.center = center;
			window.half_size = half_size;
			// Окно рисует всех, кого касается, поэтому его глубина считается по нему, а не по группе.
			_sun_group_fit_depth(group, r_bounds, center - half_size, center + half_size, p_scenario);
		}
		window.z_max = group.z_max + DEPTH_MARGIN_METERS;
		window.z_min = group.z_min - DEPTH_MARGIN_METERS;
	}
}

// lestoroer#sun: Новый владелец кеша: всё, что уже есть в мире, считается неподвижным.
void RendererSceneCull::_sun_map_activate(Scenario *p_scenario, RID p_light_instance, uint32_t p_caster_mask) {
	SunMapCache &cache = p_scenario->sun_map;
	while (cache.changed.first()) {
		cache.changed.remove(cache.changed.first());
	}
	cache.removed.clear();
	cache.dynamic.clear();
	cache.light_instance = p_light_instance;
	cache.invalidated = true;

	for (uint64_t i = 0; i < p_scenario->instance_data.size(); i++) {
		const InstanceData &idata = p_scenario->instance_data[i];
		if (!_sun_is_geometry(idata)) {
			continue;
		}
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(idata.instance->base_data);
		geom->sun_in_map = _sun_is_caster(idata, p_caster_mask);
		geom->sun_aabb = idata.instance->transformed_aabb;
		geom->sun_dynamic = false;
		if (geom->sun_in_map && _sun_is_always_dynamic(idata.instance)) {
			geom->sun_dynamic = true;
			geom->sun_dynamic_index = cache.dynamic.size();
			cache.dynamic.push_back(idata.instance);
		}
	}
}

// lestoroer#sun
bool RendererSceneCull::_sun_is_always_dynamic(const Instance *p_instance) {
	return p_instance->base_type == RSE::INSTANCE_PARTICLES || static_cast<InstanceGeometryData *>(p_instance->base_data)->material_is_animated;
}

// lestoroer#sun
void RendererSceneCull::_sun_map_remove_dynamic(SunMapCache &r_cache, Instance *p_instance) {
	InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
	Instance *last = r_cache.dynamic[r_cache.dynamic.size() - 1];
	r_cache.dynamic[geom->sun_dynamic_index] = last;
	static_cast<InstanceGeometryData *>(last->base_data)->sun_dynamic_index = geom->sun_dynamic_index;
	r_cache.dynamic.resize(r_cache.dynamic.size() - 1);
	geom->sun_dynamic = false;
}

// lestoroer#sun: Кастер, ушедший из мира, забирает своё место в карте сейчас: при следующем кадре его данных уже нет.
void RendererSceneCull::_sun_caster_removed(Instance *p_instance) {
	if (!_sun_is_geometry(p_instance)) {
		return;
	}
	Scenario *scenario = p_instance->scenario;
	SunMapCache &cache = scenario->sun_map;
	if (cache.light_instance.is_null()) {
		return;
	}
	scenario->sun_casters_version++;
	if (p_instance->sun_changed_item.in_list()) {
		cache.changed.remove(&p_instance->sun_changed_item);
	}
	InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
	if (!geom->sun_in_map) {
		return;
	}
	// Очередь не растёт без предела, пока карта не рисуется: долгая пауза заканчивается полной перерисовкой.
	static constexpr uint32_t MAX_REMOVED_CASTERS = 4096;
	if (cache.removed.size() >= MAX_REMOVED_CASTERS) {
		cache.removed.clear();
		cache.invalidated = true;
	} else if (!(sun_map_debug.ignored_changes & SUN_CHANGE_REMOVED)) {
		SunRemovedCaster removed;
		removed.aabb = geom->sun_aabb;
		removed.was_dynamic = geom->sun_dynamic;
		cache.removed.push_back(removed);
	}
	if (geom->sun_dynamic) {
		_sun_map_remove_dynamic(cache, p_instance);
	}
	geom->sun_in_map = false;
}

// lestoroer#sun: Прямоугольник кастера в окне, в плитках атласа; пустой, если кастер окна не касается. Плитки шире текселя, поэтому запас на растеризацию не нужен.
Rect2i RendererSceneCull::_sun_map_tiles_of(const SunMapFrame &p_frame, int p_window, const AABB &p_aabb) {
	const SunMapWindow &window = p_frame.windows[p_window];
	const SunPlaneRect rect = SunPlaneAxes(p_frame.x, p_frame.y).rect_of(_sun_box_of(p_aabb));
	const real_t tiles_per_meter = 1.0 / (p_frame.texel_size * p_frame.tile_size);
	const Vector2 origin = window.center - window.half_size;
	const int from_x = CLAMP(int(Math::floor((rect.min.x - origin.x) * tiles_per_meter)), 0, window.tiles.size.x);
	const int from_y = CLAMP(int(Math::floor((rect.min.y - origin.y) * tiles_per_meter)), 0, window.tiles.size.y);
	const int to_x = CLAMP(int(Math::ceil((rect.max.x - origin.x) * tiles_per_meter)), 0, window.tiles.size.x);
	const int to_y = CLAMP(int(Math::ceil((rect.max.y - origin.y) * tiles_per_meter)), 0, window.tiles.size.y);
	return Rect2i(window.tiles.position + Vector2i(from_x, from_y), Vector2i(to_x - from_x, to_y - from_y));
}

// lestoroer#sun: Прямоугольник атласа в текселях для прямоугольника плиток. Последняя плитка забирает остаток от деления размера.
Rect2i RendererSceneCull::_sun_map_texels_of(const SunMapFrame &p_frame, const Rect2i &p_tiles) {
	const Vector2i from = p_tiles.position * p_frame.tile_size;
	Vector2i to = (p_tiles.position + p_tiles.size) * p_frame.tile_size;
	if (p_tiles.position.x + p_tiles.size.x == SunTileMask::SIZE) {
		to.x = p_frame.size;
	}
	if (p_tiles.position.y + p_tiles.size.y == SunTileMask::SIZE) {
		to.y = p_frame.size;
	}
	return Rect2i(from, to - from);
}

// lestoroer#sun: Плитки, которые могут читать точки, видимые камерой: кадр, обрезанный по геометрии мира, в плоскости солнца.
void RendererSceneCull::_sun_map_view_tiles(SunTileMask &r_view, const SunMapFrame &p_frame, const AABB &p_world_bounds, const Transform3D &p_cam_transform, const Projection &p_cam_projection) {
	// Запас, чтобы плоская геометрия не давала вырожденного пересечения.
	static constexpr real_t WORLD_MARGIN_METERS = 0.05;
	const Vector3 world_min = p_world_bounds.position - Vector3(WORLD_MARGIN_METERS, WORLD_MARGIN_METERS, WORLD_MARGIN_METERS);
	const Vector3 world_max = p_world_bounds.position + p_world_bounds.size + Vector3(WORLD_MARGIN_METERS, WORLD_MARGIN_METERS, WORLD_MARGIN_METERS);

	Vector<Plane> planes = p_cam_projection.get_projection_planes(p_cam_transform);
	planes.push_back(Plane(Vector3(1, 0, 0), world_max.x));
	planes.push_back(Plane(Vector3(-1, 0, 0), -world_min.x));
	planes.push_back(Plane(Vector3(0, 1, 0), world_max.y));
	planes.push_back(Plane(Vector3(0, -1, 0), -world_min.y));
	planes.push_back(Plane(Vector3(0, 0, 1), world_max.z));
	planes.push_back(Plane(Vector3(0, 0, -1), -world_min.z));

	r_view.reset();
	const Vector<Vector3> points = Geometry3D::compute_convex_mesh_points(planes.ptr(), planes.size());
	if (points.is_empty()) {
		return;
	}

	const real_t tiles_per_meter = 1.0 / (p_frame.texel_size * p_frame.tile_size);
	// Фильтр и смещения читают карту дальше самой точки.
	const real_t margin = p_frame.margin_texels / p_frame.tile_size;

	LocalVector<Vector2> plane_points;
	plane_points.resize(points.size());
	for (int i = 0; i < points.size(); i++) {
		plane_points[i] = Vector2(p_frame.x.dot(points[i]), p_frame.y.dot(points[i]));
	}

	for (int w = 0; w < p_frame.window_count; w++) {
		const SunMapWindow &window = p_frame.windows[w];
		const Vector2 origin = window.center - window.half_size;
		const int rows = window.tiles.size.y;
		real_t row_min[SunTileMask::SIZE];
		real_t row_max[SunTileMask::SIZE];
		for (int row = 0; row < rows; row++) {
			row_min[row] = Math::INF;
			row_max[row] = -Math::INF;
		}
		// Сечение выпуклого тела строкой плиток задают его рёбра, поэтому хватает перебрать все пары вершин.
		for (uint32_t i = 0; i < plane_points.size(); i++) {
			for (uint32_t j = i; j < plane_points.size(); j++) {
				Vector2 a = (plane_points[i] - origin) * tiles_per_meter;
				Vector2 b = (plane_points[j] - origin) * tiles_per_meter;
				if (a.y > b.y) {
					SWAP(a, b);
				}
				if (b.y + margin < 0.0 || a.y - margin > rows) {
					continue;
				}
				const int first_row = CLAMP(int(Math::floor(a.y - margin)), 0, rows - 1);
				const int last_row = CLAMP(int(Math::floor(b.y + margin)), 0, rows - 1);
				for (int row = first_row; row <= last_row; row++) {
					// Часть отрезка, попавшая в строку с запасом.
					const real_t band_from = row - margin;
					const real_t band_to = row + 1 + margin;
					const real_t height = b.y - a.y;
					const real_t from_t = height > 0.0 ? CLAMP((band_from - a.y) / height, (real_t)0.0, (real_t)1.0) : 0.0;
					const real_t to_t = height > 0.0 ? CLAMP((band_to - a.y) / height, (real_t)0.0, (real_t)1.0) : 1.0;
					const real_t x_from = a.x + (b.x - a.x) * from_t;
					const real_t x_to = a.x + (b.x - a.x) * to_t;
					row_min[row] = MIN(row_min[row], MIN(x_from, x_to));
					row_max[row] = MAX(row_max[row], MAX(x_from, x_to));
				}
			}
		}
		for (int row = 0; row < rows; row++) {
			if (row_max[row] < row_min[row]) {
				continue;
			}
			const int from = CLAMP(int(Math::floor(row_min[row] - margin)), 0, window.tiles.size.x);
			const int to = CLAMP(int(Math::ceil(row_max[row] + margin)), 0, window.tiles.size.x);
			r_view.set_rect(Rect2i(window.tiles.position.x + from, window.tiles.position.y + row, to - from, 1));
		}
	}
}

// lestoroer#sun: Запас роста группы: доля её размера, но не меньше полуметра.
static constexpr real_t SUN_GROWTH_FRACTION = 0.1;
static constexpr real_t SUN_MIN_GROWTH_METERS = 0.5;

// lestoroer#sun: Растит глубину групп тех окон, которых касается изменившийся кастер. true — какая-то группа выросла.
static bool _sun_grow_window_depths(RendererSceneCull::SunCasterBounds &r_bounds, const RendererSceneCull::SunMapFrame &p_frame, const SunPlaneRect &p_rect, const SunDepthRange &p_depth, bool p_caster) {
	bool grown = false;
	for (int w = 0; w < p_frame.window_count; w++) {
		const RendererSceneCull::SunMapWindow &window = p_frame.windows[w];
		const Vector2 low = window.center - window.half_size;
		const Vector2 high = window.center + window.half_size;
		if (p_frame.window_count > 1 && (p_rect.max.x < low.x || p_rect.min.x > high.x || p_rect.max.y < low.y || p_rect.min.y > high.y)) {
			continue;
		}
		const real_t slack = MAX(SUN_MIN_GROWTH_METERS, MAX(window.half_size.x, window.half_size.y) * 2.0 * SUN_GROWTH_FRACTION);
		RendererSceneCull::SunGroup &group = r_bounds.groups[w];
		if (p_depth.low < window.z_min) {
			group.z_min = MIN(group.z_min, p_depth.low - slack);
			grown = true;
		}
		if (p_caster && p_depth.high > window.z_max) {
			group.z_max = MAX(group.z_max, p_depth.high + slack);
			grown = true;
		}
	}
	return grown;
}

// lestoroer#sun: Окно, внутри полей которого кастер лежит целиком; -1, если такого нет. Окно, куда в этом кадре уже ушёл вышедший кастер, не подходит.
static int _sun_home_window(const RendererSceneCull::SunMapFrame &p_frame, const SunPlaneRect &p_rect, real_t p_inset, const bool *p_group_moved) {
	for (int w = 0; w < p_frame.window_count; w++) {
		const RendererSceneCull::SunMapWindow &window = p_frame.windows[w];
		const Vector2 low = window.center - window.half_size + Vector2(p_inset, p_inset);
		const Vector2 high = window.center + window.half_size - Vector2(p_inset, p_inset);
		if (!p_group_moved[w] && p_rect.min.x >= low.x && p_rect.min.y >= low.y && p_rect.max.x <= high.x && p_rect.max.y <= high.y) {
			return w;
		}
	}
	return -1;
}

// lestoroer#sun: Группа для кастера, вышедшего из всех окон: попутчик рядом, новый попутчик или ближайшая группа, расширенная до него.
// Новый попутчик заводится, если окон ещё не максимум и с ним тексель меньше, чем с расширенной ближайшей группой.
static int _sun_group_for_outside_caster(RendererSceneCull::SunCasterBounds &r_bounds, const SunPlaneRect &p_rect, const Vector2 &p_plane_center, int p_current_group) {
	// Меши одной вещи выходят из окна не в один кадр и лежат рядом.
	static constexpr real_t COMPANION_JOIN_METERS = 2.0;

	if (p_current_group < r_bounds.group_count && r_bounds.groups[p_current_group].travelling) {
		return p_current_group;
	}
	for (int group = 0; group < r_bounds.group_count; group++) {
		const RendererSceneCull::SunGroup &candidate = r_bounds.groups[group];
		if (candidate.travelling && p_rect.max.x + COMPANION_JOIN_METERS >= candidate.plane_min.x && p_rect.min.x - COMPANION_JOIN_METERS <= candidate.plane_max.x && p_rect.max.y + COMPANION_JOIN_METERS >= candidate.plane_min.y && p_rect.min.y - COMPANION_JOIN_METERS <= candidate.plane_max.y) {
			return group;
		}
	}

	int nearest = 0;
	real_t nearest_distance = Math::INF;
	for (int group = 0; group < r_bounds.group_count; group++) {
		const RendererSceneCull::SunGroup &candidate = r_bounds.groups[group];
		const Vector2 closest = p_plane_center.clamp(candidate.plane_min, candidate.plane_max);
		const real_t distance = closest.distance_squared_to(p_plane_center);
		if (distance < nearest_distance) {
			nearest_distance = distance;
			nearest = group;
		}
	}
	const Vector2 nearest_size = r_bounds.groups[nearest].plane_max - r_bounds.groups[nearest].plane_min;
	const real_t slack = MAX(SUN_MIN_GROWTH_METERS, MAX(nearest_size.x, nearest_size.y) * SUN_GROWTH_FRACTION);
	const Vector2 grown_min = r_bounds.groups[nearest].plane_min.min(p_rect.min - Vector2(slack, slack));
	const Vector2 grown_max = r_bounds.groups[nearest].plane_max.max(p_rect.max + Vector2(slack, slack));

	if (r_bounds.group_count < r_bounds.settings.max_groups) {
		Vector2 sizes_if_grown[RendererSceneCull::SUN_MAX_WINDOWS];
		Vector2 sizes_with_companion[RendererSceneCull::SUN_MAX_WINDOWS];
		for (int group = 0; group < r_bounds.group_count; group++) {
			sizes_with_companion[group] = r_bounds.groups[group].plane_max - r_bounds.groups[group].plane_min;
			sizes_if_grown[group] = group == nearest ? grown_max - grown_min : sizes_with_companion[group];
		}
		sizes_with_companion[r_bounds.group_count] = (p_rect.max - p_rect.min) * (1.0 + SUN_COMPANION_SLACK_FRACTION);
		const real_t texel_if_grown = _sun_windows_texel(sizes_if_grown, r_bounds.group_count, r_bounds.settings.margin_texels, r_bounds.settings.map_size);
		const real_t texel_with_companion = _sun_windows_texel(sizes_with_companion, r_bounds.group_count + 1, r_bounds.settings.margin_texels, r_bounds.settings.map_size);
		if (texel_with_companion < texel_if_grown) {
			const int companion = r_bounds.group_count++;
			r_bounds.groups[companion] = RendererSceneCull::SunGroup();
			r_bounds.groups[companion].travelling = true;
			r_bounds.groups[companion].plane_min = p_rect.min;
			r_bounds.groups[companion].plane_max = p_rect.max;
			return companion;
		}
	}

	r_bounds.groups[nearest].plane_min = grown_min;
	r_bounds.groups[nearest].plane_max = grown_max;
	return nearest;
}

// lestoroer#sun: Кастер, вышедший из своего окна, переходит в окно-попутчика по размеру вещи с запасом, которое следует за ней.
// Другие окна при этом не меняются, поэтому плотность не мигает, когда предмет таскают туда и обратно у края группы.
// Если окон уже максимум или расширить ближайшее выгоднее, оно расширяется.
RendererSceneCull::SunGroupsChange RendererSceneCull::_sun_map_grow_for_changes(SunCasterBounds &r_bounds, const SunMapFrame &p_frame, const Scenario *p_scenario) const {
	const SunPlaneAxes axes(p_frame.x, p_frame.y);
	const Vector3 z_abs = p_frame.z.abs();
	const real_t inset = (p_frame.window_count > 1 ? SUN_RING_MARGINS_MANY_WINDOWS : SUN_RING_MARGINS_ONE_WINDOW) * p_frame.margin_texels * p_frame.texel_size;
	const int group_count_before = r_bounds.group_count;
	bool grown = false;
	bool group_moved[SUN_MAX_WINDOWS] = {};

	for (const SelfList<Instance> *item = p_scenario->sun_map.changed.first(); item; item = item->next()) {
		Instance *instance = item->self();
		if (instance->array_index < 0 || !_sun_is_geometry(instance)) {
			continue;
		}
		const AABB &aabb = instance->transformed_aabb;
		r_bounds.world_bounds.merge_with(aabb);
		const SunBox box = _sun_box_of(aabb);
		const SunPlaneRect rect = axes.rect_of(box);
		const bool caster = _sun_is_caster(p_scenario->instance_data[instance->array_index], r_bounds.settings.caster_mask);
		if (caster || r_bounds.settings.covers_receivers) {
			grown = _sun_grow_window_depths(r_bounds, p_frame, rect, _sun_depth_of(p_frame.z, z_abs, box), caster) || grown;
		}
		if (!caster) {
			continue;
		}

		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		const int home = _sun_home_window(p_frame, rect, inset, group_moved);
		if (home >= 0) {
			geom->sun_group = home;
			continue;
		}
		grown = true;
		geom->sun_group = _sun_group_for_outside_caster(r_bounds, rect, axes.center_of(box), geom->sun_group);
		group_moved[geom->sun_group] = true;
	}

	for (int group = 0; group < r_bounds.group_count; group++) {
		if (group_moved[group] && r_bounds.groups[group].travelling) {
			_sun_companion_follow_members(r_bounds, group, p_scenario);
		}
	}
	if (r_bounds.group_count != group_count_before) {
		return SUN_GROUPS_NEW_LAYOUT;
	}
	return grown ? SUN_GROUPS_RESIZED : SUN_GROUPS_KEPT;
}

// lestoroer#sun: Окно-попутчик встаёт на своих кастеров, не уменьшаясь: раскладка не меняется, пока они в него помещаются.
void RendererSceneCull::_sun_companion_follow_members(SunCasterBounds &r_bounds, int p_group, const Scenario *p_scenario) {
	const SunPlaneAxes axes(r_bounds.map_x, r_bounds.map_y);
	Vector2 members_min(Math::INF, Math::INF);
	Vector2 members_max(-Math::INF, -Math::INF);
	for (uint64_t i = 0; i < p_scenario->instance_data.size(); i++) {
		const InstanceData &idata = p_scenario->instance_data[i];
		if (!_sun_is_caster(idata, r_bounds.settings.caster_mask) || static_cast<InstanceGeometryData *>(idata.instance->base_data)->sun_group != p_group) {
			continue;
		}
		const SunPlaneRect rect = axes.rect_of(_sun_box_of(p_scenario->instance_aabbs[i]));
		members_min = members_min.min(rect.min);
		members_max = members_max.max(rect.max);
	}
	SunGroup &group = r_bounds.groups[p_group];
	const Vector2 half = (group.plane_max - group.plane_min).max((members_max - members_min) * (1.0 + SUN_COMPANION_SLACK_FRACTION)) * 0.5;
	const Vector2 middle = (members_min + members_max) * 0.5;
	group.plane_min = middle - half;
	group.plane_max = middle + half;
}

// lestoroer#sun: Сжатие и новая раскладка — только когда плотные группы заметно лучше и держатся: так предмет, который таскают у края, не дёргает плотность.
// true — группы заменены плотными, атлас раскладывается заново.
bool RendererSceneCull::_sun_map_shrink(SunCasterBounds &r_bounds, Scenario *p_scenario, const Instance *p_light, const SunMapFrame &p_frame) const {
	static constexpr uint64_t SCAN_INTERVAL_FRAMES = 30;
	static constexpr uint64_t HOLD_FRAMES = 120;
	static constexpr real_t BETTER_TEXEL_RATIO = 0.95;
	static constexpr real_t BETTER_DEPTH_RATIO = 0.7;
	static constexpr real_t STABLE_TEXEL_RATIO = 0.01;

	SunMapCache &cache = p_scenario->sun_map;
	const uint64_t now = RSG::rasterizer->get_frame_number();
	if (!cache.fit_pending || now < cache.next_fit_frame) {
		return false;
	}
	cache.next_fit_frame = now + SCAN_INTERVAL_FRAMES;

	// Перебор поворотов дорог, поэтому его откладывают, пока мир не успокоится.
	const bool world_at_rest = now - cache.last_change_frame >= SCAN_INTERVAL_FRAMES;
	SunCasterBounds tight;
	_sun_caster_bounds_fit(tight, p_scenario, r_bounds.light_basis, r_bounds.settings, r_bounds.roll_index, world_at_rest);

	SunMapFrame tight_frame;
	_sun_map_update_frame(tight_frame, tight, p_light, true, p_scenario);

	real_t current_depth = 0.0;
	real_t tight_depth = 0.0;
	for (int i = 0; i < r_bounds.group_count; i++) {
		current_depth = MAX(current_depth, r_bounds.groups[i].z_max - r_bounds.groups[i].z_min);
	}
	for (int i = 0; i < tight.group_count; i++) {
		tight_depth = MAX(tight_depth, tight.groups[i].z_max - tight.groups[i].z_min);
	}
	const bool better = tight_frame.texel_size < p_frame.texel_size * BETTER_TEXEL_RATIO || tight_depth < current_depth * BETTER_DEPTH_RATIO || tight.group_count < r_bounds.group_count;
	if (!better) {
		cache.fit_pending = !world_at_rest;
		cache.shrink_since_frame = 0;
		return false;
	}
	if (cache.shrink_since_frame == 0 || Math::abs(tight_frame.texel_size - cache.shrink_texel) > cache.shrink_texel * STABLE_TEXEL_RATIO) {
		cache.shrink_texel = tight_frame.texel_size;
		cache.shrink_since_frame = now;
		return false;
	}
	if (now - cache.shrink_since_frame < HOLD_FRAMES) {
		return false;
	}
	r_bounds = tight;
	_sun_assign_caster_groups(r_bounds, p_scenario);
	cache.shrink_since_frame = 0;
	cache.fit_pending = !world_at_rest;
	return true;
}

// lestoroer#sun
void RendererSceneCull::_light_instance_setup_caster_bounds_shadow(int p_shadow_index, Instance *p_instance, const Transform3D &p_cam_transform) {
	InstanceLightData *light = static_cast<InstanceLightData *>(p_instance->base_data);
	Scenario *scenario = p_instance->scenario;
	const Basis light_basis = p_instance->transform.basis.orthonormalized();
	SunCasterBounds &bounds = light->sun_caster_bounds;

	const bool reuse = cull.shadow_lights_total == 1 && scene_render->sun_map_supports_reuse();
	// Окна читает только шейдер Forward+ и только без полутени.
	const bool use_windows = reuse && RSG::light_storage->light_get_param(p_instance->base, RSE::LIGHT_PARAM_SIZE) <= 0.0;

	SunFitSettings settings;
	settings.caster_mask = RSG::light_storage->light_get_shadow_caster_mask(p_instance->base);
	settings.max_groups = use_windows ? sun_map_debug.max_windows : 1;
	settings.covers_receivers = !use_windows;
	settings.margin_texels = _sun_map_margin_texels(p_instance);
	settings.map_size = RSG::light_storage->get_directional_light_shadow_size(light->instance);

	const bool first_fit = bounds.casters_version == UINT64_MAX || bounds.fit_world != scenario->self || bounds.settings.caster_mask != settings.caster_mask || bounds.settings.max_groups != settings.max_groups || bounds.settings.covers_receivers != settings.covers_receivers;
	const bool direction_changed = bounds.light_basis != light_basis;
	const int current_roll = bounds.casters_version == UINT64_MAX ? -1 : bounds.roll_index;
	bounds.settings = settings;

	SunMapFrame frame;
	if (reuse) {
		frame = _sun_map_update_region(scenario, p_instance, light_basis, first_fit, direction_changed, current_roll);
	} else {
		if (first_fit || direction_changed || bounds.casters_version != scenario->sun_casters_version) {
			_sun_caster_bounds_fit(bounds, scenario, light_basis, settings, current_roll, true);
		}
		_sun_map_update_frame(frame, bounds, p_instance, true, scenario);
	}

	cull.shadow_count = p_shadow_index + 1;
	cull.shadows[p_shadow_index].light_instance = light->instance;
	cull.shadows[p_shadow_index].caster_mask = settings.caster_mask;
	cull.shadows[p_shadow_index].cascade_count = reuse ? 0 : 1;
	if (reuse) {
		// Постоянная карта не участвует в общем кулле: кастеры приходят из очереди изменений.
		cull.sun_map_light = p_instance;
	}
	_sun_setup_window_shadows(p_shadow_index, p_instance, frame, reuse, use_windows, p_cam_transform);
}

// lestoroer#sun: Область и окна постоянной карты на этот кадр: владелец кеша, подгонка, следование за регулятором, сжатие и рост по изменениям мира.
RendererSceneCull::SunMapFrame RendererSceneCull::_sun_map_update_region(Scenario *p_scenario, Instance *p_light, const Basis &p_light_basis, bool p_first_fit, bool p_direction_changed, int p_current_roll) {
	InstanceLightData *light = static_cast<InstanceLightData *>(p_light->base_data);
	SunCasterBounds &bounds = light->sun_caster_bounds;
	const SunFitSettings settings = bounds.settings;
	SunMapCache &cache = p_scenario->sun_map;
	const uint64_t now = RSG::rasterizer->get_frame_number();
	cache.used_frame = now;
	const bool owner_changed = cache.light_instance != light->instance;
	if (owner_changed) {
		_sun_map_activate(p_scenario, light->instance, settings.caster_mask);
	}
	// Атлас один на все миры: пока карта этого мира в нём не лежит, её нужно начать заново.
	if (!RSG::light_storage->directional_shadow_claim_persistent(p_scenario->self)) {
		cache.invalidated = true;
	}

	SunMapFrame frame = cache.frame;
	bool new_layout = true;
	if (p_first_fit || owner_changed) {
		// Пока кеша не было, мир мог измениться как угодно: прежние границы света нельзя считать верными.
		_sun_caster_bounds_fit(bounds, p_scenario, p_light_basis, settings, p_current_roll, true);
		_sun_assign_caster_groups(bounds, p_scenario);
	} else if (p_direction_changed) {
		_sun_caster_bounds_follow_direction(bounds, p_scenario, p_light_basis);
		cache.fit_pending = true;
		cache.last_change_frame = now;
		cache.shrink_since_frame = 0;
	} else {
		new_layout = _sun_map_shrink(bounds, p_scenario, p_light, frame);
	}
	_sun_map_update_frame(frame, bounds, p_light, new_layout, p_scenario);

	const SunGroupsChange growth = _sun_map_grow_for_changes(bounds, frame, p_scenario);
	if (growth != SUN_GROUPS_KEPT) {
		_sun_map_update_frame(frame, bounds, p_light, growth == SUN_GROUPS_NEW_LAYOUT, p_scenario);
	}
	_sun_map_note_frame_change(cache, frame);
	return frame;
}

// lestoroer#sun: Проекции окон карты: в хранилище света при постоянной карте, в кулл — при перерисовке каждый кадр.
void RendererSceneCull::_sun_setup_window_shadows(int p_shadow_index, const Instance *p_light, const SunMapFrame &p_frame, bool p_reuse, bool p_use_windows, const Transform3D &p_cam_transform) {
	// Разделитель, до которого шейдер всегда выбирает первую область и не гасит тень с расстоянием.
	static constexpr real_t NO_SPLIT_DISTANCE = 1e6;

	const RID light_instance = static_cast<const InstanceLightData *>(p_light->base_data)->instance;
	// Смещение одно на всю карту: у окон общий тексель, и предмет, перешедший в другое окно, не меняет вид тени.
	real_t bias_scale = 0.0;
	for (int w = 0; w < p_frame.window_count; w++) {
		bias_scale = MAX(bias_scale, p_frame.windows[w].z_max - p_frame.windows[w].z_min);
	}

	Rect2 atlas_rects[SUN_MAX_WINDOWS];
	Rect2 read_rects[SUN_MAX_WINDOWS];
	const real_t read_inset = p_frame.margin_texels / real_t(p_frame.size);
	for (int w = 0; w < p_frame.window_count; w++) {
		const SunMapWindow &window = p_frame.windows[w];
		const real_t z_range = window.z_max - window.z_min;

		Projection ortho_camera;
		ortho_camera.set_orthogonal(-window.half_size.x, window.half_size.x, -window.half_size.y, window.half_size.y, 0, z_range);

		Transform3D ortho_transform;
		ortho_transform.basis.set_columns(p_frame.x, p_frame.y, p_frame.z);
		ortho_transform.origin = p_frame.x * window.center.x + p_frame.y * window.center.y + p_frame.z * window.z_max;

		const real_t range_begin = window.z_max - p_frame.z.dot(p_cam_transform.origin);
		const Vector2 uv_scale(0.5 / window.half_size.x, 0.5 / window.half_size.y);

		const Rect2i texels = _sun_map_texels_of(p_frame, window.tiles);
		atlas_rects[w] = Rect2(Vector2(texels.position) / real_t(p_frame.size), Vector2(texels.size) / real_t(p_frame.size));
		read_rects[w] = atlas_rects[w].grow(-read_inset);

		if (p_reuse) {
			RSG::light_storage->light_instance_set_shadow_transform(light_instance, ortho_camera, ortho_transform, z_range, NO_SPLIT_DISTANCE, w, p_frame.texel_size, bias_scale, range_begin, uv_scale);
			continue;
		}
		Vector<Plane> light_frustum_planes;
		light_frustum_planes.resize(6);
		light_frustum_planes.write[0] = Plane(p_frame.x, window.center.x + window.half_size.x);
		light_frustum_planes.write[1] = Plane(-p_frame.x, window.half_size.x - window.center.x);
		light_frustum_planes.write[2] = Plane(p_frame.y, window.center.y + window.half_size.y);
		light_frustum_planes.write[3] = Plane(-p_frame.y, window.half_size.y - window.center.y);
		light_frustum_planes.write[4] = Plane(p_frame.z, window.z_max + 1e6);
		light_frustum_planes.write[5] = Plane(-p_frame.z, -window.z_min);

		Cull::Shadow::Cascade &cascade = cull.shadows[p_shadow_index].cascades[w];
		cascade.frustum = Frustum(light_frustum_planes);
		cascade.projection = ortho_camera;
		cascade.transform = ortho_transform;
		cascade.zfar = z_range;
		cascade.split = NO_SPLIT_DISTANCE;
		cascade.shadow_texel_size = p_frame.texel_size;
		cascade.bias_scale = bias_scale;
		cascade.range_begin = range_begin;
		cascade.uv_scale = uv_scale;
	}
	RSG::light_storage->light_instance_set_sun_windows(light_instance, p_use_windows ? p_frame.window_count : 0, atlas_rects, read_rects);
}

// lestoroer#sun: Сравнивает новый кадр карты с прежним и решает, что начать заново. Смена осей, текселя или размера атласа отменяет всё, сдвиг окна — только его.
void RendererSceneCull::_sun_map_note_frame_change(SunMapCache &r_cache, const SunMapFrame &p_frame) {
	const SunMapFrame &previous = r_cache.frame;
	if (p_frame.size != previous.size || p_frame.x != previous.x || p_frame.y != previous.y || p_frame.z != previous.z || p_frame.reverse_cull != previous.reverse_cull || p_frame.texel_size != previous.texel_size) {
		r_cache.invalidated = true;
	}
	for (int w = 0; w < p_frame.window_count; w++) {
		if (w >= previous.window_count || p_frame.windows[w] != previous.windows[w]) {
			r_cache.window_restart[w] = true;
		}
	}
	r_cache.frame = p_frame;
}

// lestoroer#sun: Отмечает плитки кастера во всех окнах, которых он касается.
void RendererSceneCull::_sun_map_mark_tiles(SunTileMask &r_mask, const SunMapFrame &p_frame, const AABB &p_aabb) {
	for (int w = 0; w < p_frame.window_count; w++) {
		r_mask.set_rect(_sun_map_tiles_of(p_frame, w, p_aabb));
	}
}

// lestoroer#sun: Переносит изменения мира в устаревшие плитки и слои кастеров. true — устаревшие плитки могли появиться и в кадре камеры.
bool RendererSceneCull::_sun_map_apply_changes(Scenario *p_scenario, uint32_t p_caster_mask) {
	SunMapCache &cache = p_scenario->sun_map;
	const uint64_t now = RSG::rasterizer->get_frame_number();
	bool stale_may_be_in_view = !cache.removed.is_empty() || cache.changed.first() != nullptr || cache.invalidated;
	stale_may_be_in_view = _sun_map_restart_windows(cache) || stale_may_be_in_view;
	_sun_map_mark_removed_casters(cache, now);
	_sun_map_mark_changed_casters(p_scenario, p_caster_mask, now);
	return _sun_map_mark_dynamic_casters(cache, now) || stale_may_be_in_view;
}

// lestoroer#sun: Окна, начатые заново, целиком устаревают. true — такие окна есть.
bool RendererSceneCull::_sun_map_restart_windows(SunMapCache &r_cache) {
	const SunMapFrame &frame = r_cache.frame;
	const bool restart_all = r_cache.invalidated || sun_map_debug.force_full;
	if (r_cache.invalidated) {
		r_cache.invalidated = false;
		r_cache.stats_invalidations++;
	}
	bool any_restart = false;
	SunTileMask occupied;
	for (int w = 0; w < frame.window_count; w++) {
		occupied.set_rect(frame.windows[w].tiles);
		r_cache.window_restart[w] = r_cache.window_restart[w] || restart_all;
		if (r_cache.window_restart[w]) {
			r_cache.atlas_stale.set_rect(frame.windows[w].tiles);
			r_cache.static_stale.set_rect(frame.windows[w].tiles);
			any_restart = true;
		}
	}
	if (any_restart) {
		// Плитки, которые перестали быть чьим-то окном, никто не читает и не обязан догонять.
		r_cache.atlas_stale.keep_mask(occupied);
		r_cache.static_stale.keep_mask(occupied);
	}
	return any_restart;
}

// lestoroer#sun: Место ушедших из мира кастеров.
void RendererSceneCull::_sun_map_mark_removed_casters(SunMapCache &r_cache, uint64_t p_now) {
	for (const SunRemovedCaster &removed : r_cache.removed) {
		_sun_map_mark_tiles(r_cache.atlas_stale, r_cache.frame, removed.aabb);
		if (!removed.was_dynamic) {
			_sun_map_mark_tiles(r_cache.static_stale, r_cache.frame, removed.aabb);
		}
		r_cache.fit_pending = true;
		r_cache.last_change_frame = p_now;
	}
	r_cache.removed.clear();
}

// lestoroer#sun: Изменившийся кастер освобождает прежнее место и становится подвижным на новом.
void RendererSceneCull::_sun_map_mark_changed_casters(Scenario *p_scenario, uint32_t p_caster_mask, uint64_t p_now) {
	SunMapCache &cache = p_scenario->sun_map;
	const SunMapFrame &frame = cache.frame;
	while (SelfList<Instance> *item = cache.changed.first()) {
		Instance *instance = item->self();
		cache.changed.remove(item);
		cache.stats_changes++;
		if (!_sun_is_geometry(instance)) {
			continue;
		}
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		const bool caster = instance->array_index >= 0 && _sun_is_caster(p_scenario->instance_data[instance->array_index], p_caster_mask);
		if (caster || geom->sun_in_map) {
			cache.fit_pending = true;
			cache.last_change_frame = p_now;
		}

		if (geom->sun_in_map) {
			_sun_map_mark_tiles(cache.atlas_stale, frame, geom->sun_aabb);
			if (!geom->sun_dynamic) {
				// Неподвижный кастер вынимается из статической карты.
				_sun_map_mark_tiles(cache.static_stale, frame, geom->sun_aabb);
			}
		}
		if (!caster) {
			if (geom->sun_dynamic) {
				_sun_map_remove_dynamic(cache, instance);
			}
			geom->sun_in_map = false;
			continue;
		}
		geom->sun_in_map = true;
		geom->sun_aabb = instance->transformed_aabb;
		geom->sun_change_frame = p_now;
		_sun_map_mark_tiles(cache.atlas_stale, frame, geom->sun_aabb);
		if (!geom->sun_dynamic) {
			geom->sun_dynamic = true;
			geom->sun_dynamic_index = cache.dynamic.size();
			cache.dynamic.push_back(instance);
		}
	}
}

// lestoroer#sun: Анимированные кастеры устаревают каждый кадр, остальные подвижные, простоявшие достаточно, становятся неподвижными.
// true — есть анимированные: устаревшие плитки могли появиться в кадре.
bool RendererSceneCull::_sun_map_mark_dynamic_casters(SunMapCache &r_cache, uint64_t p_now) {
	const SunMapFrame &frame = r_cache.frame;
	const bool ignore_animated = sun_map_debug.ignored_changes & SUN_CHANGE_ANIMATED;
	bool any_animated = false;
	for (int i = int(r_cache.dynamic.size()) - 1; i >= 0; i--) {
		Instance *instance = r_cache.dynamic[i];
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		if (_sun_is_always_dynamic(instance)) {
			any_animated = true;
			if (!ignore_animated) {
				_sun_map_mark_tiles(r_cache.atlas_stale, frame, geom->sun_aabb);
				_sun_map_mark_tiles(r_cache.atlas_stale, frame, instance->transformed_aabb);
			}
			geom->sun_aabb = instance->transformed_aabb;
		} else if (p_now - geom->sun_change_frame >= sun_map_debug.settle_frames) {
			// В атласе кастер уже есть, а в статическую карту попадёт, когда она понадобится в этих плитках.
			_sun_map_remove_dynamic(r_cache, instance);
			_sun_map_mark_tiles(r_cache.static_stale, frame, geom->sun_aabb);
		}
	}
	return any_animated;
}

// lestoroer#sun
RendererSceneRender::RenderShadowData &RendererSceneCull::_sun_map_add_step(RendererSceneRender::SunMapStep::Target p_target, RID p_light_instance, const SunMapFrame &p_frame, int p_window) {
	RendererSceneRender::RenderShadowData &step = render_shadow_data[max_shadows_used++];
	step.light = p_light_instance;
	step.pass = p_window;
	step.sun_step = RendererSceneRender::SunMapStep();
	step.sun_step.target = p_target;
	step.sun_step.window_rect = _sun_map_texels_of(p_frame, p_frame.windows[p_window].tiles);
	return step;
}

// lestoroer#sun: Кастеры, которых задевают отмеченные плитки окна. Подвижные берутся только по просьбе.
void RendererSceneCull::_sun_map_gather_casters(LocalVector<Instance *> &r_casters, const Scenario *p_scenario, const SunMapFrame &p_frame, int p_window, const SunTileMask &p_tiles, uint32_t p_caster_mask, bool p_dynamic_too) {
	r_casters.clear();
	for (uint64_t i = 0; i < p_scenario->instance_data.size(); i++) {
		const InstanceData &idata = p_scenario->instance_data[i];
		if (!_sun_is_caster(idata, p_caster_mask)) {
			continue;
		}
		const InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(idata.instance->base_data);
		if ((p_dynamic_too || !geom->sun_dynamic) && p_tiles.intersects(_sun_map_tiles_of(p_frame, p_window, geom->sun_aabb))) {
			r_casters.push_back(idata.instance);
		}
	}
}

// lestoroer#sun
void RendererSceneCull::_sun_map_add_casters_to_step(RendererSceneRender::RenderShadowData &r_step, const LocalVector<Instance *> &p_instances) {
	for (Instance *instance : p_instances) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		r_step.instances.push_back(geom->geometry_instance);
		if (instance->mesh_instance.is_valid()) {
			RSG::mesh_storage->mesh_instance_check_for_update(instance->mesh_instance);
		}
	}
}

// lestoroer#sun: Решает, что перерисовать в этом кадре. Если устаревших плиток в кадре нет, шагов нет и проход теней не выполняется.
void RendererSceneCull::_sun_map_build_steps(Scenario *p_scenario, Instance *p_light, const Transform3D &p_cam_transform, const Projection &p_cam_projection) {
	InstanceLightData *light = static_cast<InstanceLightData *>(p_light->base_data);
	SunMapCache &cache = p_scenario->sun_map;
	const SunMapFrame &frame = cache.frame;
	const uint32_t caster_mask = light->sun_caster_bounds.settings.caster_mask;

	const bool stale_may_be_in_view = _sun_map_apply_changes(p_scenario, caster_mask);

	cache.stats_last_need = Rect2i();
	cache.stats_last_static = Rect2i();
	cache.stats_last_static_casters = 0;
	cache.stats_last_dynamic_casters = 0;

	bool any_restart = false;
	for (int w = 0; w < frame.window_count; w++) {
		any_restart = any_restart || cache.window_restart[w];
	}
	if (any_restart || (cache.atlas_stale.any() && (stale_may_be_in_view || cache.checked_cam_transform != p_cam_transform || cache.checked_cam_projection != p_cam_projection))) {
		cache.checked_cam_transform = p_cam_transform;
		cache.checked_cam_projection = p_cam_projection;
		SunTileMask view;
		if (sun_map_debug.no_view_limit || sun_map_debug.force_full) {
			view.set_all();
		} else {
			_sun_map_view_tiles(view, frame, light->sun_caster_bounds.world_bounds, p_cam_transform, p_cam_projection);
		}
		for (int w = 0; w < frame.window_count; w++) {
			SunTileMask window_view = view;
			window_view.keep_rect(frame.windows[w].tiles);
			if (cache.window_restart[w]) {
				cache.window_restart[w] = false;
				_sun_map_restart_window(p_scenario, light->instance, w, window_view, caster_mask);
			} else {
				_sun_map_redraw_stale_tiles(p_scenario, light->instance, w, window_view, caster_mask);
			}
		}
	}

	if (sun_map_debug.validate) {
		_sun_map_add_validation_steps(p_scenario, light->instance, caster_mask);
	}

	RSG::mesh_storage->update_mesh_instances();
}

// lestoroer#sun: Окно начато заново: достаточно нарисовать то, что может попасть в кадр, сразу в атлас. Остальные плитки догонят позже.
void RendererSceneCull::_sun_map_restart_window(Scenario *p_scenario, RID p_light_instance, int p_window, const SunTileMask &p_window_view, uint32_t p_caster_mask) {
	SunMapCache &cache = p_scenario->sun_map;
	const SunMapFrame &frame = cache.frame;
	RendererSceneRender::RenderShadowData &step = _sun_map_add_step(RendererSceneRender::SunMapStep::TARGET_ATLAS, p_light_instance, frame, p_window);
	step.sun_step.clear_rect = step.sun_step.window_rect;
	_sun_map_gather_casters(cache.scratch_static, p_scenario, frame, p_window, p_window_view, p_caster_mask, true);
	_sun_map_add_casters_to_step(step, cache.scratch_static);
	cache.atlas_stale.clear_mask(p_window_view);
	cache.stats_last_need = frame.windows[p_window].tiles;
	cache.stats_last_static_casters += cache.scratch_static.size();
}

// lestoroer#sun: Закрывает устаревшие плитки окна, попавшие в кадр.
void RendererSceneCull::_sun_map_redraw_stale_tiles(Scenario *p_scenario, RID p_light_instance, int p_window, const SunTileMask &p_window_view, uint32_t p_caster_mask) {
	using SunMapStep = RendererSceneRender::SunMapStep;

	SunMapCache &cache = p_scenario->sun_map;
	const SunMapFrame &frame = cache.frame;
	const Rect2i window_texels = _sun_map_texels_of(frame, frame.windows[p_window].tiles);
	// Ножницы отсчитываются от области прохода.
	const auto to_window = [&](const Rect2i &p_texels) {
		return Rect2i(p_texels.position - window_texels.position, p_texels.size);
	};

	Rect2i need = cache.atlas_stale.bounds_within(p_window_view);
	if (!need.has_area()) {
		return;
	}
	// Устаревшее кольцо вокруг кадра при отъезде камеры перерисовывало бы весь кадр каждый кадр: раз платить почти всё, закрываем всё разом.
	const Rect2i all_stale = cache.atlas_stale.bounds_within(frame.windows[p_window].tiles);
	if (need.get_area() * 2 >= all_stale.get_area()) {
		need = all_stale;
	}
	cache.stats_last_need = need;

	LocalVector<Instance *> &dynamic_casters = cache.scratch_dynamic;
	dynamic_casters.clear();
	for (Instance *instance : cache.dynamic) {
		if (need.intersects(_sun_map_tiles_of(frame, p_window, static_cast<InstanceGeometryData *>(instance->base_data)->sun_aabb))) {
			dynamic_casters.push_back(instance);
		}
	}
	const Rect2i need_texels = _sun_map_texels_of(frame, need);
	const bool direct = dynamic_casters.is_empty() && cache.static_stale.covers(need);
	const Rect2i static_tiles = direct ? need : cache.static_stale.bounds_within(need);

	LocalVector<Instance *> &static_casters = cache.scratch_static;
	static_casters.clear();
	if (static_tiles.has_area()) {
		SunTileMask static_mask;
		static_mask.set_rect(static_tiles);
		_sun_map_gather_casters(static_casters, p_scenario, frame, p_window, static_mask, p_caster_mask, false);
	}
	cache.stats_last_static = static_tiles;
	cache.stats_last_static_casters += static_casters.size();
	cache.stats_last_dynamic_casters += dynamic_casters.size();

	if (direct || (static_casters.is_empty() && static_tiles == need)) {
		// Статическая карта здесь ничего не даёт: подвижных нет и неподвижные рисуются сразу в атлас, либо неподвижных нет совсем.
		RendererSceneRender::RenderShadowData &step = _sun_map_add_step(SunMapStep::TARGET_ATLAS, p_light_instance, frame, p_window);
		step.sun_step.clear_rect = need_texels;
		step.sun_step.scissor = to_window(need_texels);
		_sun_map_add_casters_to_step(step, direct ? static_casters : dynamic_casters);
	} else {
		if (static_tiles.has_area()) {
			const Rect2i static_texels = _sun_map_texels_of(frame, static_tiles);
			RendererSceneRender::RenderShadowData &step = _sun_map_add_step(SunMapStep::TARGET_STATIC, p_light_instance, frame, p_window);
			step.sun_step.clear_rect = static_texels;
			step.sun_step.scissor = to_window(static_texels);
			_sun_map_add_casters_to_step(step, static_casters);
			cache.static_stale.clear_rect(static_tiles);
		}
		RendererSceneRender::RenderShadowData &step = _sun_map_add_step(SunMapStep::TARGET_ATLAS, p_light_instance, frame, p_window);
		step.sun_step.copy_static_rect = need_texels;
		step.sun_step.scissor = to_window(need_texels);
		_sun_map_add_casters_to_step(step, dynamic_casters);
	}
	cache.atlas_stale.clear_rect(need);
}

// lestoroer#sun: Эталон: все кастеры заново в отдельную текстуру теми же окнами. Кастер, не покрытый окном, обрезан и в атласе, и в эталоне:
// побайтное сравнение его не видит, поэтому покрытие считается отдельно.
void RendererSceneCull::_sun_map_add_validation_steps(Scenario *p_scenario, RID p_light_instance, uint32_t p_caster_mask) {
	SunMapCache &cache = p_scenario->sun_map;
	const SunMapFrame &frame = cache.frame;

	// Детектор могут намеренно сломать, поэтому эталон не верит запомненным положениям кастеров.
	LocalVector<Instance *> &all_casters = cache.scratch_static;
	all_casters.clear();
	for (uint64_t i = 0; i < p_scenario->instance_data.size(); i++) {
		if (_sun_is_caster(p_scenario->instance_data[i], p_caster_mask)) {
			all_casters.push_back(p_scenario->instance_data[i].instance);
		}
	}
	for (int w = 0; w < frame.window_count; w++) {
		RendererSceneRender::RenderShadowData &step = _sun_map_add_step(RendererSceneRender::SunMapStep::TARGET_VALIDATION, p_light_instance, frame, w);
		step.sun_step.clear_rect = step.sun_step.window_rect;
		_sun_map_add_casters_to_step(step, all_casters);
	}

	// Центр окна стоит на сетке текселей и отходит от своей группы на долю текселя.
	const real_t inset = (frame.window_count > 1 ? SUN_RING_MARGINS_MANY_WINDOWS : SUN_RING_MARGINS_ONE_WINDOW) * frame.margin_texels * frame.texel_size - frame.texel_size;
	cache.stats_uncovered_casters = 0;
	const SunPlaneAxes axes(frame.x, frame.y);
	const Vector3 z_abs = frame.z.abs();
	for (Instance *caster : all_casters) {
		const SunBox box = _sun_box_of(caster->transformed_aabb);
		const SunPlaneRect rect = axes.rect_of(box);
		const SunDepthRange depth = _sun_depth_of(frame.z, z_abs, box);
		bool covered = false;
		bool in_depth = true;
		for (int w = 0; w < frame.window_count; w++) {
			const SunMapWindow &window = frame.windows[w];
			const Vector2 low = window.center - window.half_size;
			const Vector2 high = window.center + window.half_size;
			const bool touches = rect.max.x >= low.x && rect.max.y >= low.y && rect.min.x <= high.x && rect.min.y <= high.y;
			if (touches && (depth.high > window.z_max || depth.low < window.z_min)) {
				in_depth = false;
			}
			covered = covered || (rect.min.x >= low.x + inset && rect.min.y >= low.y + inset && rect.max.x <= high.x - inset && rect.max.y <= high.y - inset);
		}
		if (!covered || !in_depth) {
			cache.stats_uncovered_casters++;
		}
	}
}

// lestoroer#sun
void RendererSceneCull::sun_map_debug_set_option(const StringName &p_option, const Variant &p_value) {
	if (p_option == SNAME("force_full")) {
		sun_map_debug.force_full = p_value;
	} else if (p_option == SNAME("validate")) {
		sun_map_debug.validate = p_value;
	} else if (p_option == SNAME("no_view_limit")) {
		sun_map_debug.no_view_limit = p_value;
	} else if (p_option == SNAME("ignored_changes")) {
		sun_map_debug.ignored_changes = p_value;
	} else if (p_option == SNAME("settle_frames")) {
		sun_map_debug.settle_frames = p_value;
	} else if (p_option == SNAME("max_windows")) {
		sun_map_debug.max_windows = CLAMP(int(p_value), 1, SUN_MAX_WINDOWS);
	} else if (p_option == SNAME("measure_gpu_time")) {
		scene_render->sun_measure_gpu_time = p_value;
	} else {
		ERR_PRINT(vformat("Unknown sun map debug option: %s.", p_option));
	}
}

// lestoroer#sun
Dictionary RendererSceneCull::sun_map_debug_get_state(RID p_scenario) {
	Dictionary state = RSG::light_storage->directional_shadow_debug_get_textures();
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_NULL_V(scenario, state);
	const SunMapCache &cache = scenario->sun_map;
	state["active"] = cache.light_instance.is_valid();
	state["in_use"] = cache.light_instance.is_valid() && RSG::rasterizer->get_frame_number() - cache.used_frame <= 1;
	state["side_m"] = cache.frame.windows[0].half_size.x * 2.0;
	state["texel_mm"] = cache.frame.texel_size * 1000.0;
	state["depth_m"] = cache.frame.windows[0].z_max - cache.frame.windows[0].z_min;
	state["dynamic_casters"] = cache.dynamic.size();
	state["invalidations"] = cache.stats_invalidations;
	state["changes"] = cache.stats_changes;
	state["last_need"] = cache.stats_last_need;
	state["last_static"] = cache.stats_last_static;
	state["last_static_casters"] = cache.stats_last_static_casters;
	state["last_dynamic_casters"] = cache.stats_last_dynamic_casters;
	state["uncovered_casters"] = cache.stats_uncovered_casters;
	for (const Instance *light : scenario->directional_lights) {
		const InstanceLightData *light_data = static_cast<const InstanceLightData *>(light->base_data);
		if (light_data->instance == cache.light_instance) {
			state["world_bounds"] = light_data->sun_caster_bounds.world_bounds;
		}
	}
	Array windows;
	for (int w = 0; w < cache.frame.window_count; w++) {
		const SunMapWindow &window = cache.frame.windows[w];
		Dictionary entry;
		entry["tiles"] = window.tiles;
		entry["size_m"] = window.half_size * 2.0;
		entry["center"] = window.center;
		entry["depth_m"] = window.z_max - window.z_min;
		windows.push_back(entry);
	}
	state["windows"] = windows;
	PackedInt64Array atlas_rows;
	atlas_rows.resize(SunTileMask::SIZE);
	for (int i = 0; i < SunTileMask::SIZE; i++) {
		atlas_rows.write[i] = int64_t(cache.atlas_stale.get_row(i));
	}
	state["atlas_stale"] = atlas_rows;
	return state;
}
