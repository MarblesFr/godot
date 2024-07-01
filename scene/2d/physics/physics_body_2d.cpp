/**************************************************************************/
/*  physics_body_2d.cpp                                                   */
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

#include "physics_body_2d.h"

void PhysicsBody2D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("move_h", "amount", "collision_callback"), &PhysicsBody2D::move_h, DEFVAL(0.0f), DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("move_v", "amount", "collision_callback"), &PhysicsBody2D::move_v, DEFVAL(0.0f), DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("move_h_exact", "amount", "collision_callback"), &PhysicsBody2D::move_h_exact, DEFVAL(0), DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("move_v_exact", "amount", "collision_callback"), &PhysicsBody2D::move_v_exact, DEFVAL(0), DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("test_move", "from", "motion", "collision", "recovery_as_collision"), &PhysicsBody2D::test_move, DEFVAL(Variant()), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_gravity"), &PhysicsBody2D::get_gravity);

	ClassDB::bind_method(D_METHOD("get_collision_exceptions"), &PhysicsBody2D::get_collision_exceptions);
	ClassDB::bind_method(D_METHOD("add_collision_exception_with", "body"), &PhysicsBody2D::add_collision_exception_with);
	ClassDB::bind_method(D_METHOD("remove_collision_exception_with", "body"), &PhysicsBody2D::remove_collision_exception_with);

	ClassDB::bind_method(D_METHOD("set_position_delta", "value"), &PhysicsBody2D::set_position_delta);
	ClassDB::bind_method(D_METHOD("get_position_delta"), &PhysicsBody2D::get_position_delta);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "position_delta", PROPERTY_HINT_LAYERS_2D_PHYSICS), "set_position_delta", "get_position_delta");

	GDVIRTUAL_BIND(_move_h_exact, "amount", "collision_callback");
	GDVIRTUAL_BIND(_move_v_exact, "amount", "collision_callback");
}

PhysicsBody2D::PhysicsBody2D(PhysicsServer2D::BodyMode p_mode, PhysicsServer2D::ColliderType p_type) :
		CollisionObject2D(PhysicsServer2D::get_singleton()->body_create(), false) {
	set_body_mode(p_mode);
	set_collider_type(p_type);
	set_pickable(false);
}

bool PhysicsBody2D::move_h(real_t p_amount, const Callable &p_callback) {
	position_delta.x += p_amount;
	int whole_move = round_half_to_even(position_delta.x);
	if (whole_move == 0) {
		return false;
	}
	position_delta.x -= whole_move;
	return _move_h_exact(whole_move, p_callback);
}

bool PhysicsBody2D::move_v(real_t p_amount, const Callable &p_callback) {
	position_delta.y += p_amount;
	int whole_move = round_half_to_even(position_delta.y);
	if (whole_move == 0) {
		return false;
	}
	position_delta.y -= whole_move;
	return _move_v_exact(whole_move, p_callback);
}


bool PhysicsBody2D::test_move(const Transform2Di &p_from, const Vector2i &p_motion, const Ref<KinematicCollision2D> &r_collision, bool p_recovery_as_collision) {
	ERR_FAIL_COND_V(!is_inside_tree(), false);

	PhysicsServer2D::MotionResult *r = nullptr;
	PhysicsServer2D::MotionResult temp_result;
	if (r_collision.is_valid()) {
		// Needs const_cast because method bindings don't support non-const Ref.
		r = const_cast<PhysicsServer2D::MotionResult *>(&r_collision->result);
	} else {
		r = &temp_result;
	}

	PhysicsServer2D::MotionParameters parameters(p_from, p_motion);
	parameters.recovery_as_collision = p_recovery_as_collision;

	return PhysicsServer2D::get_singleton()->body_test_motion(get_rid(), parameters, r);
}

Vector2i PhysicsBody2D::get_gravity() const {
	PhysicsDirectBodyState2D *state = PhysicsServer2D::get_singleton()->body_get_direct_state(get_rid());
	ERR_FAIL_NULL_V(state, Vector2i());
	return state->get_total_gravity();
}

void PhysicsBody2D::set_position_delta(Vector2 p_value){
	position_delta = p_value;
}

Vector2 PhysicsBody2D::get_position_delta() const {
	return position_delta;
}

TypedArray<PhysicsBody2D> PhysicsBody2D::get_collision_exceptions() {
	List<RID> exceptions;
	PhysicsServer2D::get_singleton()->body_get_collision_exceptions(get_rid(), &exceptions);
	Array ret;
	for (const RID &body : exceptions) {
		ObjectID instance_id = PhysicsServer2D::get_singleton()->body_get_object_instance_id(body);
		Object *obj = ObjectDB::get_instance(instance_id);
		PhysicsBody2D *physics_body = Object::cast_to<PhysicsBody2D>(obj);
		ret.append(physics_body);
	}
	return ret;
}

void PhysicsBody2D::add_collision_exception_with(Node *p_node) {
	ERR_FAIL_NULL(p_node);
	PhysicsBody2D *physics_body = Object::cast_to<PhysicsBody2D>(p_node);
	ERR_FAIL_NULL_MSG(physics_body, "Collision exception only works between two nodes that inherit from PhysicsBody2D.");
	PhysicsServer2D::get_singleton()->body_add_collision_exception(get_rid(), physics_body->get_rid());
}

void PhysicsBody2D::remove_collision_exception_with(Node *p_node) {
	ERR_FAIL_NULL(p_node);
	PhysicsBody2D *physics_body = Object::cast_to<PhysicsBody2D>(p_node);
	ERR_FAIL_NULL_MSG(physics_body, "Collision exception only works between two nodes that inherit from PhysicsBody2D.");
	PhysicsServer2D::get_singleton()->body_remove_collision_exception(get_rid(), physics_body->get_rid());
}

bool PhysicsBody2D::_move_h_exact(int32_t p_amount, const Callable &p_callback) {
	bool result = false;
	if (GDVIRTUAL_CALL(_move_h_exact, p_amount, p_callback, result)) {
		return result;
	}
	return move_h_exact(p_amount, p_callback);
}

bool PhysicsBody2D::_move_v_exact(int32_t p_amount, const Callable &p_callback) {
	bool result = false;
	if (GDVIRTUAL_CALL(_move_v_exact, p_amount, p_callback, result)) {
		return result;
	}
	return move_v_exact(p_amount, p_callback);
}
