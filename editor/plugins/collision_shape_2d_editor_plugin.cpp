/**************************************************************************/
/*  collision_shape_2d_editor_plugin.cpp                                  */
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

#include "collision_shape_2d_editor_plugin.h"

#include "canvas_item_editor_plugin.h"
#include "core/os/keyboard.h"
#include "editor/editor_node.h"
#include "editor/editor_settings.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/resources/2d/rectangle_shape_2d.h"
#include "scene/resources/2d/segment_shape_2d.h"
#include "scene/resources/2d/separation_ray_shape_2d.h"
#include "scene/resources/2d/world_boundary_shape_2d.h"

CollisionShape2DEditor::CollisionShape2DEditor() {
	grab_threshold = EDITOR_GET("editors/polygon_editor/point_grab_radius");
}

void CollisionShape2DEditor::_node_removed(Node *p_node) {
	if (p_node == node) {
		node = nullptr;
	}
}

Variant CollisionShape2DEditor::get_handle_value(int idx) const {
	switch (shape_type) {
		case WORLD_BOUNDARY_SHAPE: {
			Ref<WorldBoundaryShape2D> world_boundary = node->get_shape();

			if (idx == 0) {
				return world_boundary->get_distance();
			} else {
				return world_boundary->get_normal();
			}

		} break;

		case SEPARATION_RAY_SHAPE: {
			Ref<SeparationRayShape2D> ray = node->get_shape();

			if (idx == 0) {
				return ray->get_length();
			}

		} break;

		case RECTANGLE_SHAPE: {
			Ref<RectangleShape2D> rect = node->get_shape();

			if (idx < 8) {
				return rect->get_offset() + rect->get_size().abs();
			}

		} break;

		case SEGMENT_SHAPE: {
			Ref<SegmentShape2D> seg = node->get_shape();

			if (idx == 0) {
				return seg->get_a();
			} else if (idx == 1) {
				return seg->get_b();
			}

		} break;
	}

	return Variant();
}

void CollisionShape2DEditor::set_handle(int idx, Point2i &p_point) {
	switch (shape_type) {
		case WORLD_BOUNDARY_SHAPE: {
			if (idx < 2) {
				Ref<WorldBoundaryShape2D> world_boundary = node->get_shape();

				if (idx == 0) {
					Vector2 normal = world_boundary->get_normal();
					world_boundary->set_distance(p_point.dot(normal) / normal.length_squared());
				} else {
					world_boundary->set_normal(p_point / (world_boundary->get_distance() + 30));
				}
			}
		} break;

		case SEPARATION_RAY_SHAPE: {
			Ref<SeparationRayShape2D> ray = node->get_shape();

			ray->set_length(Math::abs(p_point.y));
		} break;

		case RECTANGLE_SHAPE: {
			if (idx < 8) {
				Ref<RectangleShape2D> rect = node->get_shape();
				Vector2i size = (Point2i)original;

				Vector2i new_position_delta = Vector2i();

				int y_delta = 0;
				int new_height = 0;
				if (RECT_HANDLES[idx].y != 0) {
					int half_height = size.y / 2;
					if ((idx == 1 || idx == 2 || idx == 3) && size.y % 2 == 1) {
						half_height += 1;
					}
					y_delta = p_point.y - half_height * RECT_HANDLES[idx].y;
					new_height = size.y + y_delta * RECT_HANDLES[idx].y;
					if (new_height < 1) {
						y_delta -= (new_height - 1) * RECT_HANDLES[idx].y;
						new_height = 1;
					}
				}

				int x_delta = 0;
				int new_width = 0;
				if (RECT_HANDLES[idx].x != 0) {
					int half_width = size.x / 2;
					if ((idx == 7 || idx == 0 || idx == 1) && size.x % 2 == 1) {
						half_width += 1;
					}
					x_delta = p_point.x - half_width * RECT_HANDLES[idx].x;
					new_width = size.x + x_delta * RECT_HANDLES[idx].x;
					if (new_width < 1) {
						x_delta -= (new_width - 1) * RECT_HANDLES[idx].x;
						new_width = 1;
					}
				}

				if (y_delta != 0) {
					if (new_height % 2 == 1 && size.y % 2 == 0) {
						new_position_delta.y += ((y_delta - 1) / 2);
					} else if (new_height % 2 == 0 && size.y % 2 == 1) {
						new_position_delta.y += ((y_delta + 1) / 2);
					} else {
						new_position_delta.y += (y_delta / 2);
					}
					size.y = new_height;
				}

				if (x_delta != 0) {
					if (new_width % 2 == 1 && size.x % 2 == 0) {
						new_position_delta.x += ((x_delta - 1) / 2);
					} else if (new_width % 2 == 0 && size.x % 2 == 1) {
						new_position_delta.x += ((x_delta + 1) / 2);
					} else {
						new_position_delta.x += (x_delta / 2);
					}
					size.x = new_width;
				}

				if (Input::get_singleton()->is_key_pressed(Key::ALT)) {
					rect->set_size(((Point2i)original + ((size - (Point2i)original) * 2)).abs());
					node->set_global_position(original_transform.get_origin());
				} else {
					rect->set_size(size.abs());
					Point2i pos = original_transform.affine_inverse().xform(original_transform.get_origin());
					pos += new_position_delta;
					node->set_global_position(original_transform.xform(pos));
				}
			}

		} break;

		case SEGMENT_SHAPE: {
			if (edit_handle < 2) {
				Ref<SegmentShape2D> seg = node->get_shape();

				if (idx == 0) {
					seg->set_a(p_point);
				} else if (idx == 1) {
					seg->set_b(p_point);
				}
			}
		} break;
	}
}

void CollisionShape2DEditor::commit_handle(int idx, Variant &p_org) {
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Set Handle"));

	switch (shape_type) {
		case WORLD_BOUNDARY_SHAPE: {
			Ref<WorldBoundaryShape2D> world_boundary = node->get_shape();

			if (idx == 0) {
				undo_redo->add_do_method(world_boundary.ptr(), "set_distance", world_boundary->get_distance());
				undo_redo->add_undo_method(world_boundary.ptr(), "set_distance", p_org);
			} else {
				undo_redo->add_do_method(world_boundary.ptr(), "set_normal", world_boundary->get_normal());
				undo_redo->add_undo_method(world_boundary.ptr(), "set_normal", p_org);
			}

		} break;

		case SEPARATION_RAY_SHAPE: {
			Ref<SeparationRayShape2D> ray = node->get_shape();

			undo_redo->add_do_method(ray.ptr(), "set_length", ray->get_length());
			undo_redo->add_undo_method(ray.ptr(), "set_length", p_org);

		} break;

		case RECTANGLE_SHAPE: {
			Ref<RectangleShape2D> rect = node->get_shape();

			undo_redo->add_do_method(rect.ptr(), "set_size", rect->get_size());
//			undo_redo->add_do_method(rect.ptr(), "set_offset", rect->get_offset());
			undo_redo->add_do_method(node, "set_global_transform", node->get_global_transform());
			undo_redo->add_undo_method(rect.ptr(), "set_size", p_org);
//			undo_redo->add_undo_method(rect.ptr(), "set_offset", p_org);
			undo_redo->add_undo_method(node, "set_global_transform", original_transform);

		} break;

		case SEGMENT_SHAPE: {
			Ref<SegmentShape2D> seg = node->get_shape();
			if (idx == 0) {
				undo_redo->add_do_method(seg.ptr(), "set_a", seg->get_a());
				undo_redo->add_undo_method(seg.ptr(), "set_a", p_org);
			} else if (idx == 1) {
				undo_redo->add_do_method(seg.ptr(), "set_b", seg->get_b());
				undo_redo->add_undo_method(seg.ptr(), "set_b", p_org);
			}

		} break;
	}

	undo_redo->commit_action();
}

bool CollisionShape2DEditor::forward_canvas_gui_input(const Ref<InputEvent> &p_event) {
	if (!node) {
		return false;
	}

	if (!node->is_visible_in_tree()) {
		return false;
	}

	if (shape_type == -1) {
		return false;
	}

	Ref<InputEventMouseButton> mb = p_event;
	Transform2D xform = canvas_item_editor->get_canvas_transform() * node->get_global_transform();

	if (mb.is_valid()) {
		Vector2 gpoint = mb->get_position();

		if (mb->get_button_index() == MouseButton::LEFT) {
			if (mb->is_pressed()) {
				for (int i = 0; i < handles.size(); i++) {
					if (xform.xform(handles[i]).distance_to(gpoint) < grab_threshold) {
						edit_handle = i;

						break;
					}
				}

				if (edit_handle == -1) {
					pressed = false;

					return false;
				}

				original_mouse_pos = gpoint;
				original_point = handles[edit_handle];
				original = get_handle_value(edit_handle);
				original_transform = node->get_global_transform();
				last_point = original;
				pressed = true;

				return true;

			} else {
				if (pressed) {
					if (original_mouse_pos != gpoint) {
						commit_handle(edit_handle, original);
					}

					edit_handle = -1;
					pressed = false;

					return true;
				}
			}
		}

		return false;
	}

	Ref<InputEventMouseMotion> mm = p_event;

	if (mm.is_valid()) {
		if (edit_handle == -1 || !pressed) {
			return false;
		}

		Vector2i cpoint = canvas_item_editor->snap_point(canvas_item_editor->get_canvas_transform().affine_inverse().xform(mm->get_position()));
		cpoint = original_transform.affine_inverse().xform(cpoint);
		last_point = cpoint;

		set_handle(edit_handle, cpoint);

		return true;
	}

	Ref<InputEventKey> k = p_event;

	if (k.is_valid()) {
		if (edit_handle == -1 || !pressed || k->is_echo()) {
			return false;
		}

		if (shape_type == RECTANGLE_SHAPE && k->get_keycode() == Key::ALT) {
			set_handle(edit_handle, last_point); // Update handle when Alt key is toggled.
		}
	}

	return false;
}

void CollisionShape2DEditor::_shape_changed() {
	canvas_item_editor->update_viewport();

	if (current_shape.is_valid()) {
		current_shape->disconnect_changed(callable_mp(canvas_item_editor, &CanvasItemEditor::update_viewport));
		current_shape = Ref<Shape2D>();
		shape_type = -1;
	}

	if (!node) {
		return;
	}

	current_shape = node->get_shape();

	if (current_shape.is_valid()) {
		current_shape->connect_changed(callable_mp(canvas_item_editor, &CanvasItemEditor::update_viewport));
	} else {
		return;
	}

	if (Object::cast_to<WorldBoundaryShape2D>(*current_shape)) {
		shape_type = WORLD_BOUNDARY_SHAPE;
	} else if (Object::cast_to<SeparationRayShape2D>(*current_shape)) {
		shape_type = SEPARATION_RAY_SHAPE;
	} else if (Object::cast_to<RectangleShape2D>(*current_shape)) {
		shape_type = RECTANGLE_SHAPE;
	} else if (Object::cast_to<SegmentShape2D>(*current_shape)) {
		shape_type = SEGMENT_SHAPE;
	}
}

void CollisionShape2DEditor::forward_canvas_draw_over_viewport(Control *p_overlay) {
	if (!node) {
		return;
	}

	if (!node->is_visible_in_tree()) {
		return;
	}

	if (shape_type == -1) {
		return;
	}

	Transform2D gt = canvas_item_editor->get_canvas_transform() * node->get_global_transform();

	Ref<Texture2D> h = get_editor_theme_icon(SNAME("EditorHandle"));
	Vector2 size = h->get_size() * 0.5;

	handles.clear();

	switch (shape_type) {
		case WORLD_BOUNDARY_SHAPE: {
			Ref<WorldBoundaryShape2D> shape = current_shape;

			handles.resize(2);
			handles.write[0] = shape->get_normal() * shape->get_distance();
			handles.write[1] = shape->get_normal() * (shape->get_distance() + 30.0);

			p_overlay->draw_texture(h, gt.xform(handles[0]) - size);
			p_overlay->draw_texture(h, gt.xform(handles[1]) - size);

		} break;

		case SEPARATION_RAY_SHAPE: {
			Ref<SeparationRayShape2D> shape = current_shape;

			handles.resize(1);
			handles.write[0] = Point2(0, shape->get_length());

			p_overlay->draw_texture(h, gt.xform(handles[0]) - size);

		} break;

		case RECTANGLE_SHAPE: {
			Ref<RectangleShape2D> shape = current_shape;

			handles.resize(8);
			Vector2i rect_size = shape->get_size();
			Vector2i offset = shape->get_offset();
			for (int i = 0; i < handles.size(); i++) {
				handles.write[i] = offset + (Vector2(RECT_HANDLES[i] * rect_size) * 0.5).ceil();
				p_overlay->draw_texture(h, gt.xform(handles[i]) - size);
			}

		} break;

		case SEGMENT_SHAPE: {
			Ref<SegmentShape2D> shape = current_shape;

			handles.resize(2);
			handles.write[0] = shape->get_a();
			handles.write[1] = shape->get_b();

			p_overlay->draw_texture(h, gt.xform(handles[0]) - size);
			p_overlay->draw_texture(h, gt.xform(handles[1]) - size);

		} break;
	}
}

void CollisionShape2DEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			get_tree()->connect("node_removed", callable_mp(this, &CollisionShape2DEditor::_node_removed));
		} break;

		case NOTIFICATION_EXIT_TREE: {
			get_tree()->disconnect("node_removed", callable_mp(this, &CollisionShape2DEditor::_node_removed));
		} break;

		case NOTIFICATION_PROCESS: {
			if (node && node->get_shape() != current_shape) {
				_shape_changed();
			}
		} break;

		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			if (EditorSettings::get_singleton()->check_changed_settings_in_group("editors/polygon_editor")) {
				grab_threshold = EDITOR_GET("editors/polygon_editor/point_grab_radius");
			}
		} break;
	}
}

void CollisionShape2DEditor::edit(Node *p_node) {
	if (!canvas_item_editor) {
		canvas_item_editor = CanvasItemEditor::get_singleton();
	}

	if (p_node) {
		node = Object::cast_to<CollisionShape2D>(p_node);
		set_process(true);
	} else {
		if (pressed) {
			set_handle(edit_handle, original_point);
			pressed = false;
		}
		edit_handle = -1;
		node = nullptr;
		set_process(false);
	}
	_shape_changed();
}

void CollisionShape2DEditorPlugin::edit(Object *p_obj) {
	collision_shape_2d_editor->edit(Object::cast_to<Node>(p_obj));
}

bool CollisionShape2DEditorPlugin::handles(Object *p_obj) const {
	return p_obj->is_class("CollisionShape2D");
}

void CollisionShape2DEditorPlugin::make_visible(bool visible) {
	if (!visible) {
		edit(nullptr);
	}
}

CollisionShape2DEditorPlugin::CollisionShape2DEditorPlugin() {
	collision_shape_2d_editor = memnew(CollisionShape2DEditor);
	EditorNode::get_singleton()->get_gui_base()->add_child(collision_shape_2d_editor);
}

CollisionShape2DEditorPlugin::~CollisionShape2DEditorPlugin() {
}
