/**************************************************************************/
/*  godot_shape_2d.cpp                                                    */
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

#include "godot_shape_2d.h"

#include "core/math/geometry_2d.h"
#include "core/templates/sort_array.h"

void GodotShape2D::configure(const Rect2i &p_aabb) {
	aabb = p_aabb;
	configured = true;
	for (const KeyValue<GodotShapeOwner2D *, int> &E : owners) {
		GodotShapeOwner2D *co = const_cast<GodotShapeOwner2D *>(E.key);
		co->_shape_changed();
	}
}

Vector2i GodotShape2D::get_support(const Vector2i &p_normal) const {
	Vector2i res[2];
	int amnt;
	get_supports(p_normal, res, amnt);
	return res[0];
}

void GodotShape2D::add_owner(GodotShapeOwner2D *p_owner) {
	HashMap<GodotShapeOwner2D *, int>::Iterator E = owners.find(p_owner);
	if (E) {
		E->value++;
	} else {
		owners[p_owner] = 1;
	}
}

void GodotShape2D::remove_owner(GodotShapeOwner2D *p_owner) {
	HashMap<GodotShapeOwner2D *, int>::Iterator E = owners.find(p_owner);
	ERR_FAIL_COND(!E);
	E->value--;
	if (E->value == 0) {
		owners.remove(E);
	}
}

bool GodotShape2D::is_owner(GodotShapeOwner2D *p_owner) const {
	return owners.has(p_owner);
}

const HashMap<GodotShapeOwner2D *, int> &GodotShape2D::get_owners() const {
	return owners;
}

GodotShape2D::~GodotShape2D() {
	ERR_FAIL_COND(owners.size());
}

/*********************************************************/
/*********************************************************/
/*********************************************************/

void GodotWorldBoundaryShape2D::get_supports(const Vector2i &p_normal, Vector2i *r_supports, int &r_amount) const {
	r_amount = 0;
}

bool GodotWorldBoundaryShape2D::contains_point(const Vector2i &p_point) const {
	return normal.dot(p_point) < d;
}

bool GodotWorldBoundaryShape2D::intersect_segment(const Vector2i &p_begin, const Vector2i &p_end, Vector2i &r_point, Vector2i &r_normal) const {
	Vector2i segment = p_begin - p_end;
	real_t den = normal.dot(segment);

	//printf("den is %i\n",den);
	if (Math::abs(den) <= CMP_EPSILON) {
		return false;
	}

	real_t dist = (normal.dot(p_begin) - d) / den;
	//printf("dist is %i\n",dist);

	if (dist < -CMP_EPSILON || dist > (1.0 + CMP_EPSILON)) {
		return false;
	}

	r_point = p_begin + segment * -dist;
	r_normal = normal;

	return true;
}

real_t GodotWorldBoundaryShape2D::get_moment_of_inertia(real_t p_mass, const Size2 &p_scale) const {
	return 0;
}

void GodotWorldBoundaryShape2D::set_data(const Variant &p_data) {
	ERR_FAIL_COND(p_data.get_type() != Variant::ARRAY);
	Array arr = p_data;
	ERR_FAIL_COND(arr.size() != 2);
	normal = arr[0];
	d = arr[1];
	configure(Rect2(Vector2i(-1e15, -1e15), Vector2i(1e15 * 2, 1e15 * 2)));
}

Variant GodotWorldBoundaryShape2D::get_data() const {
	Array arr;
	arr.resize(2);
	arr[0] = normal;
	arr[1] = d;
	return arr;
}

/*********************************************************/
/*********************************************************/
/*********************************************************/

void GodotSeparationRayShape2D::get_supports(const Vector2i &p_normal, Vector2i *r_supports, int &r_amount) const {
	r_amount = 1;

	if (p_normal.y > 0) {
		*r_supports = Vector2i(0, length);
	} else {
		*r_supports = Vector2i();
	}
}

bool GodotSeparationRayShape2D::contains_point(const Vector2i &p_point) const {
	return false;
}

bool GodotSeparationRayShape2D::intersect_segment(const Vector2i &p_begin, const Vector2i &p_end, Vector2i &r_point, Vector2i &r_normal) const {
	return false; //rays can't be intersected
}

real_t GodotSeparationRayShape2D::get_moment_of_inertia(real_t p_mass, const Size2 &p_scale) const {
	return 0; //rays are mass-less
}

void GodotSeparationRayShape2D::set_data(const Variant &p_data) {
	Dictionary d = p_data;
	length = d["length"];
	slide_on_slope = d["slide_on_slope"];
	configure(Rect2(0, 0, 0.001, length));
}

Variant GodotSeparationRayShape2D::get_data() const {
	Dictionary d;
	d["length"] = length;
	d["slide_on_slope"] = slide_on_slope;
	return d;
}

/*********************************************************/
/*********************************************************/
/*********************************************************/

void GodotSegmentShape2D::get_supports(const Vector2i &p_normal, Vector2i *r_supports, int &r_amount) const {
	if (Math::abs(p_normal.dot(n)) > segment_is_valid_support_threshold) {
		r_supports[0] = a;
		r_supports[1] = b;
		r_amount = 2;
		return;
	}

	real_t dp = p_normal.dot(b - a);
	if (dp > 0) {
		*r_supports = b;
	} else {
		*r_supports = a;
	}
	r_amount = 1;
}

bool GodotSegmentShape2D::contains_point(const Vector2i &p_point) const {
	return false;
}

bool GodotSegmentShape2D::intersect_segment(const Vector2i &p_begin, const Vector2i &p_end, Vector2i &r_point, Vector2i &r_normal) const {
	if (!Geometry2D::segment_intersects_segment(p_begin, p_end, a, b, &r_point)) {
		return false;
	}

	if (n.dot(p_begin) > n.dot(a)) {
		r_normal = n;
	} else {
		r_normal = -n;
	}

	return true;
}

real_t GodotSegmentShape2D::get_moment_of_inertia(real_t p_mass, const Size2 &p_scale) const {
	return p_mass * ((a * p_scale).distance_squared_to(b * p_scale)) / 12;
}

void GodotSegmentShape2D::set_data(const Variant &p_data) {
	ERR_FAIL_COND(p_data.get_type() != Variant::RECT2);

	Rect2 r = p_data;
	a = r.position;
	b = r.size;
	n = (b - a).orthogonal();

	Rect2 aabb_new;
	aabb_new.position = a;
	aabb_new.expand_to(b);
	if (aabb_new.size.x == 0) {
		aabb_new.size.x = 0.001;
	}
	if (aabb_new.size.y == 0) {
		aabb_new.size.y = 0.001;
	}
	configure(aabb_new);
}

Variant GodotSegmentShape2D::get_data() const {
	Rect2 r;
	r.position = a;
	r.size = b;
	return r;
}

/*********************************************************/
/*********************************************************/
/*********************************************************/

void GodotRectangleShape2D::get_supports(const Vector2i &p_normal, Vector2i *r_supports, int &r_amount) const {
	for (int i = 0; i < 2; i++) {
		Vector2i ag;
		ag[i] = 1.0;
		real_t dp = ag.dot(p_normal);
		if (Math::abs(dp) <= segment_is_valid_support_threshold) {
			continue;
		}

		Vector2i half_extents = dp > 0 ? half_extents_br : half_extents_tl;

		r_amount = 2;

		r_supports[0][i] = offset.x + half_extents[i];
		r_supports[0][i ^ 1] = offset.y + half_extents_br[i ^ 1];

		r_supports[1][i] = offset.x + half_extents[i];
		r_supports[1][i ^ 1] = offset.y + half_extents_tl[i ^ 1];

		return;
	}

	/* USE POINT */

	r_amount = 1;
	r_supports[0] = Vector2i(
			offset.x +
			((p_normal.x < 0) ? half_extents_tl.x : half_extents_br.x),
			offset.y +
			((p_normal.y < 0) ? half_extents_tl.y : half_extents_br.y));
}

bool GodotRectangleShape2D::contains_point(const Vector2i &p_point) const {
	real_t x = p_point.x;
	real_t y = p_point.y;
	return (x >= offset.x + half_extents_tl.x) && (x < offset.x + half_extents_br.x) && (y >= offset.y + half_extents_tl.y) && (y < offset.y + half_extents_br.y);
}

bool GodotRectangleShape2D::intersect_segment(const Vector2i &p_begin, const Vector2i &p_end, Vector2i &r_point, Vector2i &r_normal) const {
	return get_aabb().intersects_segment(p_begin, p_end, &r_point, &r_normal);
}

real_t GodotRectangleShape2D::get_moment_of_inertia(real_t p_mass, const Size2 &p_scale) const {
	Vector2i he2 = size * p_scale;
	return p_mass * he2.dot(he2) / 12.0;
}

void GodotRectangleShape2D::set_data(const Variant &p_data) {
	ERR_FAIL_COND(p_data.get_type() != Variant::ARRAY);

	Array arr = p_data;
	ERR_FAIL_COND(arr.size() != 2);

	size = arr[0];
	offset = arr[1];
	half_extents_tl = -(size / 2);
	half_extents_br = ((size + Vector2i(1, 1)) / 2);
	configure(Rect2(offset + half_extents_tl, size));
}

Variant GodotRectangleShape2D::get_data() const {
	Array arr;
	arr.resize(2);
	arr[0] = size;
	arr[1] = offset;
	return arr;
}
