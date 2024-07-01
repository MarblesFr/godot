/**************************************************************************/
/*  character_body_2d.cpp                                                 */
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

#include "character_body_2d.h"

void CharacterBody2D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("on_ground"), &CharacterBody2D::on_ground);
	ClassDB::bind_method(D_METHOD("collides_at", "delta", "result"), &CharacterBody2D::_collides_at, DEFVAL(Variant()));

	GDVIRTUAL_BIND(_is_riding, "solid");
	GDVIRTUAL_BIND(_squish);
}

CharacterBody2D::CharacterBody2D() :
		PhysicsBody2D(PhysicsServer2D::BODY_MODE_KINEMATIC, PhysicsServer2D::COLLIDER_TYPE_ACTOR) {
	PhysicsServer2D::get_singleton()->body_set_is_riding(get_rid(), callable_mp(this, &CharacterBody2D::_is_riding));
}

bool CharacterBody2D::move_h_exact(int32_t amount, const Callable &collision_callback) {
	int move_dir = SIGN(amount);
	Vector2i move_dir_vector = Vector2i(move_dir, 0);
	int amount_moved = 0;
	PhysicsServer2D::CollisionResult r_result;
	while (amount != 0)
	{
		bool colliding = collides_at(move_dir_vector, &r_result);
		if (colliding)
		{
			if (collision_callback.is_valid())
			{
				if(collision_callback.call(move_dir_vector, amount_moved, amount, r_result.collider)) {
					position_delta.x = 0;
				}
			}
			else {
				position_delta.x = 0;
			}
			return true;
		}
		amount_moved += move_dir;
		amount -= move_dir;
		translate(move_dir_vector);
	}
	return false;
}

bool CharacterBody2D::move_v_exact(int32_t amount, const Callable &collision_callback) {
	int move_dir = SIGN(amount);
	Vector2i move_dir_vector = Vector2i(0, move_dir);
	int amount_moved = 0;
	PhysicsServer2D::CollisionResult r_result;
	while (amount != 0)
	{
		bool colliding = collides_at(move_dir_vector, &r_result);
		if (colliding)
		{
			if (collision_callback.is_valid())
			{
				if(collision_callback.call(move_dir_vector, amount_moved, amount, r_result.collider)) {
					position_delta.y = 0;
				}
			}
			else {
				position_delta.y = 0;
			}
			return true;
		}
		amount_moved += move_dir;
		amount -= move_dir;
		translate(move_dir_vector);
	}
	return false;
}

bool CharacterBody2D::collides_at(const Vector2i &delta, PhysicsServer2D::CollisionResult *r_result) {
	return PhysicsServer2D::get_singleton()->body_collides_at(get_rid(), get_global_transform_i(), delta, r_result);
}

bool CharacterBody2D::_collides_at(const Vector2i &delta, const Ref<PhysicsCollisionResult2D> &p_result) {
	PhysicsServer2D::CollisionResult *result_ptr = nullptr;
	if (p_result.is_valid()) {
		result_ptr = p_result->get_result_ptr();
	}

	return collides_at(delta, result_ptr);
}

bool CharacterBody2D::on_ground() {
	return collides_at(Vector2i(0, 1));
}

bool CharacterBody2D::_is_riding(const RID &p_solid) {
	bool result = false;
	GDVIRTUAL_CALL(_is_riding, p_solid, result);
	return result;
}

void CharacterBody2D::_squish() {
	GDVIRTUAL_CALL(_squish);
}
