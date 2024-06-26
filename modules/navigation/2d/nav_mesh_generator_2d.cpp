/**************************************************************************/
/*  nav_mesh_generator_2d.cpp                                             */
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

#ifdef CLIPPER2_ENABLED

#include "nav_mesh_generator_2d.h"

#include "core/config/project_settings.h"
#include "scene/2d/mesh_instance_2d.h"
#include "scene/2d/multimesh_instance_2d.h"
#include "scene/2d/navigation_obstacle_2d.h"
#include "scene/2d/physics/static_body_2d.h"
#include "scene/2d/polygon_2d.h"
#include "scene/2d/tile_map.h"
#include "scene/resources/2d/navigation_mesh_source_geometry_data_2d.h"
#include "scene/resources/2d/navigation_polygon.h"
#include "scene/resources/2d/rectangle_shape_2d.h"

#include "thirdparty/clipper2/include/clipper2/clipper.h"
#include "thirdparty/misc/polypartition.h"

NavMeshGenerator2D *NavMeshGenerator2D::singleton = nullptr;
Mutex NavMeshGenerator2D::baking_navmesh_mutex;
Mutex NavMeshGenerator2D::generator_task_mutex;
RWLock NavMeshGenerator2D::generator_rid_rwlock;
bool NavMeshGenerator2D::use_threads = true;
bool NavMeshGenerator2D::baking_use_multiple_threads = true;
bool NavMeshGenerator2D::baking_use_high_priority_threads = true;
HashSet<Ref<NavigationPolygon>> NavMeshGenerator2D::baking_navmeshes;
HashMap<WorkerThreadPool::TaskID, NavMeshGenerator2D::NavMeshGeneratorTask2D *> NavMeshGenerator2D::generator_tasks;
RID_Owner<NavMeshGenerator2D::NavMeshGeometryParser2D> NavMeshGenerator2D::generator_parser_owner;
LocalVector<NavMeshGenerator2D::NavMeshGeometryParser2D *> NavMeshGenerator2D::generator_parsers;

NavMeshGenerator2D *NavMeshGenerator2D::get_singleton() {
	return singleton;
}

NavMeshGenerator2D::NavMeshGenerator2D() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;

	baking_use_multiple_threads = GLOBAL_GET("navigation/baking/thread_model/baking_use_multiple_threads");
	baking_use_high_priority_threads = GLOBAL_GET("navigation/baking/thread_model/baking_use_high_priority_threads");

	// Using threads might cause problems on certain exports or with the Editor on certain devices.
	// This is the main switch to turn threaded navmesh baking off should the need arise.
	use_threads = baking_use_multiple_threads;
}

NavMeshGenerator2D::~NavMeshGenerator2D() {
	cleanup();
}

void NavMeshGenerator2D::sync() {
	if (generator_tasks.size() == 0) {
		return;
	}

	baking_navmesh_mutex.lock();
	generator_task_mutex.lock();

	LocalVector<WorkerThreadPool::TaskID> finished_task_ids;

	for (KeyValue<WorkerThreadPool::TaskID, NavMeshGeneratorTask2D *> &E : generator_tasks) {
		if (WorkerThreadPool::get_singleton()->is_task_completed(E.key)) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(E.key);
			finished_task_ids.push_back(E.key);

			NavMeshGeneratorTask2D *generator_task = E.value;
			DEV_ASSERT(generator_task->status == NavMeshGeneratorTask2D::TaskStatus::BAKING_FINISHED);

			baking_navmeshes.erase(generator_task->navigation_mesh);
			if (generator_task->callback.is_valid()) {
				generator_emit_callback(generator_task->callback);
			}
			memdelete(generator_task);
		}
	}

	for (WorkerThreadPool::TaskID finished_task_id : finished_task_ids) {
		generator_tasks.erase(finished_task_id);
	}

	generator_task_mutex.unlock();
	baking_navmesh_mutex.unlock();
}

void NavMeshGenerator2D::cleanup() {
	baking_navmesh_mutex.lock();
	generator_task_mutex.lock();

	baking_navmeshes.clear();

	for (KeyValue<WorkerThreadPool::TaskID, NavMeshGeneratorTask2D *> &E : generator_tasks) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(E.key);
		NavMeshGeneratorTask2D *generator_task = E.value;
		memdelete(generator_task);
	}
	generator_tasks.clear();

	generator_rid_rwlock.write_lock();
	for (NavMeshGeometryParser2D *parser : generator_parsers) {
		generator_parser_owner.free(parser->self);
	}
	generator_parsers.clear();
	generator_rid_rwlock.write_unlock();

	generator_task_mutex.unlock();
	baking_navmesh_mutex.unlock();
}

void NavMeshGenerator2D::finish() {
	cleanup();
}

void NavMeshGenerator2D::parse_source_geometry_data(Ref<NavigationPolygon> p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, Node *p_root_node, const Callable &p_callback) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(!p_navigation_mesh.is_valid());
	ERR_FAIL_NULL(p_root_node);
	ERR_FAIL_COND(!p_root_node->is_inside_tree());
	ERR_FAIL_COND(!p_source_geometry_data.is_valid());

	generator_parse_source_geometry_data(p_navigation_mesh, p_source_geometry_data, p_root_node);

	if (p_callback.is_valid()) {
		generator_emit_callback(p_callback);
	}
}

void NavMeshGenerator2D::bake_from_source_geometry_data(Ref<NavigationPolygon> p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, const Callable &p_callback) {
	ERR_FAIL_COND(!p_navigation_mesh.is_valid());
	ERR_FAIL_COND(!p_source_geometry_data.is_valid());

	if (p_navigation_mesh->get_outline_count() == 0 && !p_source_geometry_data->has_data()) {
		p_navigation_mesh->clear();
		if (p_callback.is_valid()) {
			generator_emit_callback(p_callback);
		}
		return;
	}

	if (is_baking(p_navigation_mesh)) {
		ERR_FAIL_MSG("NavigationPolygon is already baking. Wait for current bake to finish.");
	}
	baking_navmesh_mutex.lock();
	baking_navmeshes.insert(p_navigation_mesh);
	baking_navmesh_mutex.unlock();

	generator_bake_from_source_geometry_data(p_navigation_mesh, p_source_geometry_data);

	baking_navmesh_mutex.lock();
	baking_navmeshes.erase(p_navigation_mesh);
	baking_navmesh_mutex.unlock();

	if (p_callback.is_valid()) {
		generator_emit_callback(p_callback);
	}
}

void NavMeshGenerator2D::bake_from_source_geometry_data_async(Ref<NavigationPolygon> p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, const Callable &p_callback) {
	ERR_FAIL_COND(!p_navigation_mesh.is_valid());
	ERR_FAIL_COND(!p_source_geometry_data.is_valid());

	if (p_navigation_mesh->get_outline_count() == 0 && !p_source_geometry_data->has_data()) {
		p_navigation_mesh->clear();
		if (p_callback.is_valid()) {
			generator_emit_callback(p_callback);
		}
		return;
	}

	if (!use_threads) {
		bake_from_source_geometry_data(p_navigation_mesh, p_source_geometry_data, p_callback);
		return;
	}

	if (is_baking(p_navigation_mesh)) {
		ERR_FAIL_MSG("NavigationPolygon is already baking. Wait for current bake to finish.");
	}
	baking_navmesh_mutex.lock();
	baking_navmeshes.insert(p_navigation_mesh);
	baking_navmesh_mutex.unlock();

	generator_task_mutex.lock();
	NavMeshGeneratorTask2D *generator_task = memnew(NavMeshGeneratorTask2D);
	generator_task->navigation_mesh = p_navigation_mesh;
	generator_task->source_geometry_data = p_source_geometry_data;
	generator_task->callback = p_callback;
	generator_task->status = NavMeshGeneratorTask2D::TaskStatus::BAKING_STARTED;
	generator_task->thread_task_id = WorkerThreadPool::get_singleton()->add_native_task(&NavMeshGenerator2D::generator_thread_bake, generator_task, NavMeshGenerator2D::baking_use_high_priority_threads, "NavMeshGeneratorBake2D");
	generator_tasks.insert(generator_task->thread_task_id, generator_task);
	generator_task_mutex.unlock();
}

bool NavMeshGenerator2D::is_baking(Ref<NavigationPolygon> p_navigation_polygon) {
	baking_navmesh_mutex.lock();
	bool baking = baking_navmeshes.has(p_navigation_polygon);
	baking_navmesh_mutex.unlock();
	return baking;
}

void NavMeshGenerator2D::generator_thread_bake(void *p_arg) {
	NavMeshGeneratorTask2D *generator_task = static_cast<NavMeshGeneratorTask2D *>(p_arg);

	generator_bake_from_source_geometry_data(generator_task->navigation_mesh, generator_task->source_geometry_data);

	generator_task->status = NavMeshGeneratorTask2D::TaskStatus::BAKING_FINISHED;
}

void NavMeshGenerator2D::generator_parse_geometry_node(Ref<NavigationPolygon> p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, Node *p_node, bool p_recurse_children) {
	generator_parse_meshinstance2d_node(p_navigation_mesh, p_source_geometry_data, p_node);
	generator_parse_multimeshinstance2d_node(p_navigation_mesh, p_source_geometry_data, p_node);
	generator_parse_polygon2d_node(p_navigation_mesh, p_source_geometry_data, p_node);
	generator_parse_staticbody2d_node(p_navigation_mesh, p_source_geometry_data, p_node);
	generator_parse_tile_map_layer_node(p_navigation_mesh, p_source_geometry_data, p_node);
	generator_parse_navigationobstacle_node(p_navigation_mesh, p_source_geometry_data, p_node);

	generator_rid_rwlock.read_lock();
	for (const NavMeshGeometryParser2D *parser : generator_parsers) {
		if (!parser->callback.is_valid()) {
			continue;
		}
		parser->callback.call(p_navigation_mesh, p_source_geometry_data, p_node);
	}
	generator_rid_rwlock.read_unlock();

	if (p_recurse_children) {
		for (int i = 0; i < p_node->get_child_count(); i++) {
			generator_parse_geometry_node(p_navigation_mesh, p_source_geometry_data, p_node->get_child(i), p_recurse_children);
		}
	} else if (Object::cast_to<TileMap>(p_node)) {
		// Special case for TileMap, so that internal layer get parsed even if p_recurse_children is false.
		for (int i = 0; i < p_node->get_child_count(); i++) {
			TileMapLayer *tile_map_layer = Object::cast_to<TileMapLayer>(p_node->get_child(i));
			if (tile_map_layer && tile_map_layer->get_index_in_tile_map() >= 0) {
				generator_parse_tile_map_layer_node(p_navigation_mesh, p_source_geometry_data, tile_map_layer);
			}
		}
	}
}

void NavMeshGenerator2D::generator_parse_meshinstance2d_node(const Ref<NavigationPolygon> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, Node *p_node) {
	MeshInstance2D *mesh_instance = Object::cast_to<MeshInstance2D>(p_node);

	if (mesh_instance == nullptr) {
		return;
	}

	NavigationPolygon::ParsedGeometryType parsed_geometry_type = p_navigation_mesh->get_parsed_geometry_type();

	if (!(parsed_geometry_type == NavigationPolygon::PARSED_GEOMETRY_MESH_INSTANCES || parsed_geometry_type == NavigationPolygon::PARSED_GEOMETRY_BOTH)) {
		return;
	}

	Ref<Mesh> mesh = mesh_instance->get_mesh();
	if (!mesh.is_valid()) {
		return;
	}

	const Transform2D mesh_instance_xform = p_source_geometry_data->root_node_transform * mesh_instance->get_global_transform();

	using namespace Clipper2Lib;

	PathsD subject_paths, dummy_clip_paths;

	for (int i = 0; i < mesh->get_surface_count(); i++) {
		if (mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}

		if (!(mesh->surface_get_format(i) & Mesh::ARRAY_FLAG_USE_2D_VERTICES)) {
			continue;
		}

		PathD subject_path;

		int index_count = 0;
		if (mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_INDEX) {
			index_count = mesh->surface_get_array_index_len(i);
		} else {
			index_count = mesh->surface_get_array_len(i);
		}

		ERR_CONTINUE((index_count == 0 || (index_count % 3) != 0));

		Array a = mesh->surface_get_arrays(i);

		Vector<Vector2> mesh_vertices = a[Mesh::ARRAY_VERTEX];

		if (mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_INDEX) {
			Vector<int> mesh_indices = a[Mesh::ARRAY_INDEX];
			for (int vertex_index : mesh_indices) {
				const Vector2 &vertex = mesh_vertices[vertex_index];
				const PointD &point = PointD(vertex.x, vertex.y);
				subject_path.push_back(point);
			}
		} else {
			for (const Vector2 &vertex : mesh_vertices) {
				const PointD &point = PointD(vertex.x, vertex.y);
				subject_path.push_back(point);
			}
		}
		subject_paths.push_back(subject_path);
	}

	PathsD path_solution;

	path_solution = Union(subject_paths, dummy_clip_paths, FillRule::NonZero);

	//path_solution = RamerDouglasPeucker(path_solution, 0.025);

	Vector<Vector<Vector2>> polypaths;

	for (const PathD &scaled_path : path_solution) {
		Vector<Vector2> shape_outline;
		for (const PointD &scaled_point : scaled_path) {
			shape_outline.push_back(Point2(static_cast<real_t>(scaled_point.x), static_cast<real_t>(scaled_point.y)));
		}

		for (int i = 0; i < shape_outline.size(); i++) {
			shape_outline.write[i] = mesh_instance_xform.xform(shape_outline[i]);
		}

		p_source_geometry_data->add_obstruction_outline(shape_outline);
	}
}

void NavMeshGenerator2D::generator_parse_multimeshinstance2d_node(const Ref<NavigationPolygon> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, Node *p_node) {
	MultiMeshInstance2D *multimesh_instance = Object::cast_to<MultiMeshInstance2D>(p_node);

	if (multimesh_instance == nullptr) {
		return;
	}

	NavigationPolygon::ParsedGeometryType parsed_geometry_type = p_navigation_mesh->get_parsed_geometry_type();
	if (!(parsed_geometry_type == NavigationPolygon::PARSED_GEOMETRY_MESH_INSTANCES || parsed_geometry_type == NavigationPolygon::PARSED_GEOMETRY_BOTH)) {
		return;
	}

	Ref<MultiMesh> multimesh = multimesh_instance->get_multimesh();
	if (!(multimesh.is_valid() && multimesh->get_transform_format() == MultiMesh::TRANSFORM_2D)) {
		return;
	}

	Ref<Mesh> mesh = multimesh->get_mesh();
	if (!mesh.is_valid()) {
		return;
	}

	using namespace Clipper2Lib;

	PathsD mesh_subject_paths, dummy_clip_paths;

	for (int i = 0; i < mesh->get_surface_count(); i++) {
		if (mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}

		if (!(mesh->surface_get_format(i) & Mesh::ARRAY_FLAG_USE_2D_VERTICES)) {
			continue;
		}

		PathD subject_path;

		int index_count = 0;
		if (mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_INDEX) {
			index_count = mesh->surface_get_array_index_len(i);
		} else {
			index_count = mesh->surface_get_array_len(i);
		}

		ERR_CONTINUE((index_count == 0 || (index_count % 3) != 0));

		Array a = mesh->surface_get_arrays(i);

		Vector<Vector2> mesh_vertices = a[Mesh::ARRAY_VERTEX];

		if (mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_INDEX) {
			Vector<int> mesh_indices = a[Mesh::ARRAY_INDEX];
			for (int vertex_index : mesh_indices) {
				const Vector2 &vertex = mesh_vertices[vertex_index];
				const PointD &point = PointD(vertex.x, vertex.y);
				subject_path.push_back(point);
			}
		} else {
			for (const Vector2 &vertex : mesh_vertices) {
				const PointD &point = PointD(vertex.x, vertex.y);
				subject_path.push_back(point);
			}
		}
		mesh_subject_paths.push_back(subject_path);
	}

	PathsD mesh_path_solution = Union(mesh_subject_paths, dummy_clip_paths, FillRule::NonZero);

	//path_solution = RamerDouglasPeucker(path_solution, 0.025);

	int multimesh_instance_count = multimesh->get_visible_instance_count();
	if (multimesh_instance_count == -1) {
		multimesh_instance_count = multimesh->get_instance_count();
	}

	const Transform2D multimesh_instance_xform = p_source_geometry_data->root_node_transform * multimesh_instance->get_global_transform();

	for (int i = 0; i < multimesh_instance_count; i++) {
		const Transform2D multimesh_instance_mesh_instance_xform = multimesh_instance_xform * multimesh->get_instance_transform_2d(i);

		for (const PathD &mesh_path : mesh_path_solution) {
			Vector<Vector2> shape_outline;

			for (const PointD &mesh_path_point : mesh_path) {
				shape_outline.push_back(Point2(static_cast<real_t>(mesh_path_point.x), static_cast<real_t>(mesh_path_point.y)));
			}

			for (int j = 0; j < shape_outline.size(); j++) {
				shape_outline.write[j] = multimesh_instance_mesh_instance_xform.xform(shape_outline[j]);
			}
			p_source_geometry_data->add_obstruction_outline(shape_outline);
		}
	}
}

void NavMeshGenerator2D::generator_parse_polygon2d_node(const Ref<NavigationPolygon> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, Node *p_node) {
	Polygon2D *polygon_2d = Object::cast_to<Polygon2D>(p_node);

	if (polygon_2d == nullptr) {
		return;
	}

	NavigationPolygon::ParsedGeometryType parsed_geometry_type = p_navigation_mesh->get_parsed_geometry_type();

	if (parsed_geometry_type == NavigationPolygon::PARSED_GEOMETRY_MESH_INSTANCES || parsed_geometry_type == NavigationPolygon::PARSED_GEOMETRY_BOTH) {
		const Transform2D polygon_2d_xform = p_source_geometry_data->root_node_transform * polygon_2d->get_global_transform();

		Vector<Vector2> shape_outline = polygon_2d->get_polygon();
		for (int i = 0; i < shape_outline.size(); i++) {
			shape_outline.write[i] = polygon_2d_xform.xform(shape_outline[i]);
		}

		p_source_geometry_data->add_obstruction_outline(shape_outline);
	}
}

void NavMeshGenerator2D::generator_parse_staticbody2d_node(const Ref<NavigationPolygon> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, Node *p_node) {
	StaticBody2D *static_body = Object::cast_to<StaticBody2D>(p_node);

	if (static_body == nullptr) {
		return;
	}

	NavigationPolygon::ParsedGeometryType parsed_geometry_type = p_navigation_mesh->get_parsed_geometry_type();
	if (!(parsed_geometry_type == NavigationPolygon::PARSED_GEOMETRY_STATIC_COLLIDERS || parsed_geometry_type == NavigationPolygon::PARSED_GEOMETRY_BOTH)) {
		return;
	}

	uint32_t parsed_collision_mask = p_navigation_mesh->get_parsed_collision_mask();
	if (!(static_body->get_collision_layer() & parsed_collision_mask)) {
		return;
	}

	List<uint32_t> shape_owners;
	static_body->get_shape_owners(&shape_owners);

	for (uint32_t shape_owner : shape_owners) {
		if (static_body->is_shape_owner_disabled(shape_owner)) {
			continue;
		}

		const int shape_count = static_body->shape_owner_get_shape_count(shape_owner);

		for (int shape_index = 0; shape_index < shape_count; shape_index++) {
			Ref<Shape2D> s = static_body->shape_owner_get_shape(shape_owner, shape_index);

			if (s.is_null()) {
				continue;
			}

			const Transform2D static_body_xform = p_source_geometry_data->root_node_transform * static_body->get_global_transform() * transform2d_from_transform2di(static_body->shape_owner_get_transform(shape_owner));

			RectangleShape2D *rectangle_shape = Object::cast_to<RectangleShape2D>(*s);
			if (rectangle_shape) {
				Vector<Vector2> shape_outline;

				const Vector2i &rectangle_size = rectangle_shape->get_size();
				const Vector2i &rectangle_offset = rectangle_shape->get_offset();

				shape_outline.resize(5);
				shape_outline.write[0] = static_body_xform.xform(rectangle_offset + -(rectangle_size / 2));
				shape_outline.write[1] = static_body_xform.xform(rectangle_offset + Vector2i(rectangle_size.x + 1, -rectangle_size.y) / 2);
				shape_outline.write[2] = static_body_xform.xform(rectangle_offset + (rectangle_size + Vector2i(1, 1)) / 2);
				shape_outline.write[3] = static_body_xform.xform(rectangle_offset + Vector2i(-rectangle_size.x, rectangle_size.y + 1) / 2);
				shape_outline.write[4] = static_body_xform.xform(rectangle_offset + -(rectangle_size / 2));

				p_source_geometry_data->add_obstruction_outline(shape_outline);
			}
		}
	}
}

void NavMeshGenerator2D::generator_parse_tile_map_layer_node(const Ref<NavigationPolygon> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, Node *p_node) {
	TileMapLayer *tile_map_layer = Object::cast_to<TileMapLayer>(p_node);
	if (tile_map_layer == nullptr) {
		return;
	}

	Ref<TileSet> tile_set = tile_map_layer->get_tile_set();
	if (!tile_set.is_valid()) {
		return;
	}

	int physics_layers_count = tile_set->get_physics_layers_count();
	int navigation_layers_count = tile_set->get_navigation_layers_count();
	if (physics_layers_count <= 0 && navigation_layers_count <= 0) {
		return;
	}

	NavigationPolygon::ParsedGeometryType parsed_geometry_type = p_navigation_mesh->get_parsed_geometry_type();
	uint32_t parsed_collision_mask = p_navigation_mesh->get_parsed_collision_mask();

	const Transform2D tilemap_xform = p_source_geometry_data->root_node_transform * tile_map_layer->get_global_transform();

	TypedArray<Vector2i> used_cells = tile_map_layer->get_used_cells();
	for (int used_cell_index = 0; used_cell_index < used_cells.size(); used_cell_index++) {
		const Vector2i &cell = used_cells[used_cell_index];

		const TileData *tile_data = tile_map_layer->get_cell_tile_data(cell);
		if (tile_data == nullptr) {
			continue;
		}

		// Transform flags.
		const int alternative_id = tile_map_layer->get_cell_alternative_tile(cell);
		bool flip_h = (alternative_id & TileSetAtlasSource::TRANSFORM_FLIP_H);
		bool flip_v = (alternative_id & TileSetAtlasSource::TRANSFORM_FLIP_V);
		bool transpose = (alternative_id & TileSetAtlasSource::TRANSFORM_TRANSPOSE);

		Transform2D tile_transform;
		tile_transform.set_origin(tile_map_layer->map_to_local(cell));

		const Transform2D tile_transform_offset = tilemap_xform * tile_transform;

		// Parse traversable polygons.
		for (int navigation_layer = 0; navigation_layer < navigation_layers_count; navigation_layer++) {
			Ref<NavigationPolygon> navigation_polygon = tile_data->get_navigation_polygon(navigation_layer, flip_h, flip_v, transpose);
			if (navigation_polygon.is_valid()) {
				for (int outline_index = 0; outline_index < navigation_polygon->get_outline_count(); outline_index++) {
					const Vector<Vector2> &navigation_polygon_outline = navigation_polygon->get_outline(outline_index);
					if (navigation_polygon_outline.is_empty()) {
						continue;
					}

					Vector<Vector2> traversable_outline;
					traversable_outline.resize(navigation_polygon_outline.size());

					const Vector2 *navigation_polygon_outline_ptr = navigation_polygon_outline.ptr();
					Vector2 *traversable_outline_ptrw = traversable_outline.ptrw();

					for (int traversable_outline_index = 0; traversable_outline_index < traversable_outline.size(); traversable_outline_index++) {
						traversable_outline_ptrw[traversable_outline_index] = tile_transform_offset.xform(navigation_polygon_outline_ptr[traversable_outline_index]);
					}

					p_source_geometry_data->_add_traversable_outline(traversable_outline);
				}
			}
		}

		// Parse obstacles.
		for (int physics_layer = 0; physics_layer < physics_layers_count; physics_layer++) {
			if ((parsed_geometry_type == NavigationPolygon::PARSED_GEOMETRY_STATIC_COLLIDERS || parsed_geometry_type == NavigationPolygon::PARSED_GEOMETRY_BOTH) &&
					(tile_set->get_physics_layer_collision_layer(physics_layer) & parsed_collision_mask)) {
				for (int collision_polygon_index = 0; collision_polygon_index < tile_data->get_collision_rectangles_count(physics_layer); collision_polygon_index++) {
					PackedVector2iArray collision_polygon_data = tile_data->get_collision_rectangle_data(physics_layer, collision_polygon_index);
//					if (collision_polygon_points.is_empty()) {
//						continue;
//					}
					Vector2i size = collision_polygon_data[0];
					Vector2i offset = collision_polygon_data[1];

					if (flip_h || flip_v || transpose) {
						offset = TileData::get_transformed_offset(offset, flip_h, flip_v, transpose);
					}

					PackedVector2iArray collision_polygon_points;
					collision_polygon_points.resize(4);
					collision_polygon_points.write[0] = offset + -(size / 2);
					collision_polygon_points.write[1] = offset + Vector2i(size.x + 1, -size.y) / 2;
					collision_polygon_points.write[2] = offset + (size + Vector2i(1, 1)) / 2;
					collision_polygon_points.write[3] = offset + Vector2i(-size.x, size.y + 1) / 2;

					Vector<Vector2> obstruction_outline;
					obstruction_outline.resize(collision_polygon_points.size());

					const Vector2i *collision_polygon_points_ptr = collision_polygon_points.ptr();
					Vector2 *obstruction_outline_ptrw = obstruction_outline.ptrw();

					for (int obstruction_outline_index = 0; obstruction_outline_index < obstruction_outline.size(); obstruction_outline_index++) {
						obstruction_outline_ptrw[obstruction_outline_index] = tile_transform_offset.xform(collision_polygon_points_ptr[obstruction_outline_index]);
					}

					p_source_geometry_data->_add_obstruction_outline(obstruction_outline);
				}
			}
		}
	}
}

void NavMeshGenerator2D::generator_parse_navigationobstacle_node(const Ref<NavigationPolygon> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, Node *p_node) {
	NavigationObstacle2D *obstacle = Object::cast_to<NavigationObstacle2D>(p_node);
	if (obstacle == nullptr) {
		return;
	}

	if (!obstacle->get_affect_navigation_mesh()) {
		return;
	}

	const Transform2D node_xform = p_source_geometry_data->root_node_transform * Transform2D(0.0, obstacle->get_global_position());

	const float obstacle_radius = obstacle->get_radius();

	if (obstacle_radius > 0.0) {
		Vector<Vector2> obstruction_circle_vertices;

		// The point of this is that the moving obstacle can make a simple hole in the navigation mesh and affect the pathfinding.
		// Without, navigation paths can go directly through the middle of the obstacle and conflict with the avoidance to get agents stuck.
		// No place for excessive "round" detail here. Every additional edge adds a high cost for something that needs to be quick, not pretty.
		static const int circle_points = 12;

		obstruction_circle_vertices.resize(circle_points);
		Vector2 *circle_vertices_ptrw = obstruction_circle_vertices.ptrw();
		const real_t circle_point_step = Math_TAU / circle_points;

		for (int i = 0; i < circle_points; i++) {
			const float angle = i * circle_point_step;
			circle_vertices_ptrw[i] = node_xform.xform(Vector2(Math::cos(angle) * obstacle_radius, Math::sin(angle) * obstacle_radius));
		}

		p_source_geometry_data->add_projected_obstruction(obstruction_circle_vertices, obstacle->get_carve_navigation_mesh());
	}

	const Vector<Vector2> &obstacle_vertices = obstacle->get_vertices();

	if (obstacle_vertices.is_empty()) {
		return;
	}

	Vector<Vector2> obstruction_shape_vertices;
	obstruction_shape_vertices.resize(obstacle_vertices.size());

	const Vector2 *obstacle_vertices_ptr = obstacle_vertices.ptr();
	Vector2 *obstruction_shape_vertices_ptrw = obstruction_shape_vertices.ptrw();

	for (int i = 0; i < obstacle_vertices.size(); i++) {
		obstruction_shape_vertices_ptrw[i] = node_xform.xform(obstacle_vertices_ptr[i]);
	}
	p_source_geometry_data->add_projected_obstruction(obstruction_shape_vertices, obstacle->get_carve_navigation_mesh());
}

void NavMeshGenerator2D::generator_parse_source_geometry_data(Ref<NavigationPolygon> p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data, Node *p_root_node) {
	List<Node *> parse_nodes;

	if (p_navigation_mesh->get_source_geometry_mode() == NavigationPolygon::SOURCE_GEOMETRY_ROOT_NODE_CHILDREN) {
		parse_nodes.push_back(p_root_node);
	} else {
		p_root_node->get_tree()->get_nodes_in_group(p_navigation_mesh->get_source_geometry_group_name(), &parse_nodes);
	}

	Transform2D root_node_transform = Transform2D();
	if (Object::cast_to<Node2D>(p_root_node)) {
		root_node_transform = Object::cast_to<Node2D>(p_root_node)->get_global_transform().affine_inverse();
	}

	p_source_geometry_data->clear();
	p_source_geometry_data->root_node_transform = root_node_transform;

	bool recurse_children = p_navigation_mesh->get_source_geometry_mode() != NavigationPolygon::SOURCE_GEOMETRY_GROUPS_EXPLICIT;

	for (Node *E : parse_nodes) {
		generator_parse_geometry_node(p_navigation_mesh, p_source_geometry_data, E, recurse_children);
	}
};

static void generator_recursive_process_polytree_items(List<TPPLPoly> &p_tppl_in_polygon, const Clipper2Lib::PolyPathD *p_polypath_item) {
	using namespace Clipper2Lib;

	Vector<Vector2> polygon_vertices;

	for (const PointD &polypath_point : p_polypath_item->Polygon()) {
		polygon_vertices.push_back(Vector2(static_cast<real_t>(polypath_point.x), static_cast<real_t>(polypath_point.y)));
	}

	TPPLPoly tp;
	tp.Init(polygon_vertices.size());
	for (int j = 0; j < polygon_vertices.size(); j++) {
		tp[j] = polygon_vertices[j];
	}

	if (p_polypath_item->IsHole()) {
		tp.SetOrientation(TPPL_ORIENTATION_CW);
		tp.SetHole(true);
	} else {
		tp.SetOrientation(TPPL_ORIENTATION_CCW);
	}
	p_tppl_in_polygon.push_back(tp);

	for (size_t i = 0; i < p_polypath_item->Count(); i++) {
		const PolyPathD *polypath_item = p_polypath_item->Child(i);
		generator_recursive_process_polytree_items(p_tppl_in_polygon, polypath_item);
	}
}

bool NavMeshGenerator2D::generator_emit_callback(const Callable &p_callback) {
	ERR_FAIL_COND_V(!p_callback.is_valid(), false);

	Callable::CallError ce;
	Variant result;
	p_callback.callp(nullptr, 0, result, ce);

	return ce.error == Callable::CallError::CALL_OK;
}

RID NavMeshGenerator2D::source_geometry_parser_create() {
	RWLockWrite write_lock(generator_rid_rwlock);

	RID rid = generator_parser_owner.make_rid();

	NavMeshGeometryParser2D *parser = generator_parser_owner.get_or_null(rid);
	parser->self = rid;

	generator_parsers.push_back(parser);

	return rid;
}

void NavMeshGenerator2D::source_geometry_parser_set_callback(RID p_parser, const Callable &p_callback) {
	RWLockWrite write_lock(generator_rid_rwlock);

	NavMeshGeometryParser2D *parser = generator_parser_owner.get_or_null(p_parser);
	ERR_FAIL_NULL(parser);

	parser->callback = p_callback;
}

bool NavMeshGenerator2D::owns(RID p_object) {
	RWLockRead read_lock(generator_rid_rwlock);
	return generator_parser_owner.owns(p_object);
}

void NavMeshGenerator2D::free(RID p_object) {
	RWLockWrite write_lock(generator_rid_rwlock);

	if (generator_parser_owner.owns(p_object)) {
		NavMeshGeometryParser2D *parser = generator_parser_owner.get_or_null(p_object);

		generator_parsers.erase(parser);

		generator_parser_owner.free(p_object);
	} else {
		ERR_PRINT("Attempted to free a NavMeshGenerator2D RID that did not exist (or was already freed).");
	}
}

void NavMeshGenerator2D::generator_bake_from_source_geometry_data(Ref<NavigationPolygon> p_navigation_mesh, Ref<NavigationMeshSourceGeometryData2D> p_source_geometry_data) {
	if (p_navigation_mesh.is_null() || p_source_geometry_data.is_null()) {
		return;
	}

	if (p_navigation_mesh->get_outline_count() == 0 && !p_source_geometry_data->has_data()) {
		return;
	}

	int outline_count = p_navigation_mesh->get_outline_count();

	Vector<Vector<Vector2>> traversable_outlines;
	Vector<Vector<Vector2>> obstruction_outlines;
	Vector<NavigationMeshSourceGeometryData2D::ProjectedObstruction> projected_obstructions;

	p_source_geometry_data->get_data(
			traversable_outlines,
			obstruction_outlines,
			projected_obstructions);

	if (outline_count == 0 && traversable_outlines.size() == 0) {
		return;
	}

	using namespace Clipper2Lib;

	PathsD traversable_polygon_paths;
	PathsD obstruction_polygon_paths;

	traversable_polygon_paths.reserve(outline_count + traversable_outlines.size());
	obstruction_polygon_paths.reserve(obstruction_outlines.size());

	for (int i = 0; i < outline_count; i++) {
		const Vector<Vector2> &traversable_outline = p_navigation_mesh->get_outline(i);
		PathD subject_path;
		subject_path.reserve(traversable_outline.size());
		for (const Vector2 &traversable_point : traversable_outline) {
			const PointD &point = PointD(traversable_point.x, traversable_point.y);
			subject_path.push_back(point);
		}
		traversable_polygon_paths.push_back(subject_path);
	}

	for (const Vector<Vector2> &traversable_outline : traversable_outlines) {
		PathD subject_path;
		subject_path.reserve(traversable_outline.size());
		for (const Vector2 &traversable_point : traversable_outline) {
			const PointD &point = PointD(traversable_point.x, traversable_point.y);
			subject_path.push_back(point);
		}
		traversable_polygon_paths.push_back(subject_path);
	}

	for (const Vector<Vector2> &obstruction_outline : obstruction_outlines) {
		PathD clip_path;
		clip_path.reserve(obstruction_outline.size());
		for (const Vector2 &obstruction_point : obstruction_outline) {
			const PointD &point = PointD(obstruction_point.x, obstruction_point.y);
			clip_path.push_back(point);
		}
		obstruction_polygon_paths.push_back(clip_path);
	}

	if (!projected_obstructions.is_empty()) {
		for (const NavigationMeshSourceGeometryData2D::ProjectedObstruction &projected_obstruction : projected_obstructions) {
			if (projected_obstruction.carve) {
				continue;
			}
			if (projected_obstruction.vertices.is_empty() || projected_obstruction.vertices.size() % 2 != 0) {
				continue;
			}

			PathD clip_path;
			clip_path.reserve(projected_obstruction.vertices.size() / 2);
			for (int i = 0; i < projected_obstruction.vertices.size() / 2; i++) {
				const PointD &point = PointD(projected_obstruction.vertices[i * 2], projected_obstruction.vertices[i * 2 + 1]);
				clip_path.push_back(point);
			}
			if (!IsPositive(clip_path)) {
				std::reverse(clip_path.begin(), clip_path.end());
			}
			obstruction_polygon_paths.push_back(clip_path);
		}
	}

	Rect2 baking_rect = p_navigation_mesh->get_baking_rect();
	if (baking_rect.has_area()) {
		Vector2 baking_rect_offset = p_navigation_mesh->get_baking_rect_offset();

		const int rect_begin_x = baking_rect.position[0] + baking_rect_offset.x;
		const int rect_begin_y = baking_rect.position[1] + baking_rect_offset.y;
		const int rect_end_x = baking_rect.position[0] + baking_rect.size[0] + baking_rect_offset.x;
		const int rect_end_y = baking_rect.position[1] + baking_rect.size[1] + baking_rect_offset.y;

		RectD clipper_rect = RectD(rect_begin_x, rect_begin_y, rect_end_x, rect_end_y);

		traversable_polygon_paths = RectClip(clipper_rect, traversable_polygon_paths);
		obstruction_polygon_paths = RectClip(clipper_rect, obstruction_polygon_paths);
	}

	PathsD path_solution;

	// first merge all traversable polygons according to user specified fill rule
	PathsD dummy_clip_path;
	traversable_polygon_paths = Union(traversable_polygon_paths, dummy_clip_path, FillRule::NonZero);
	// merge all obstruction polygons, don't allow holes for what is considered "solid" 2D geometry
	obstruction_polygon_paths = Union(obstruction_polygon_paths, dummy_clip_path, FillRule::NonZero);

	path_solution = Difference(traversable_polygon_paths, obstruction_polygon_paths, FillRule::NonZero);

	real_t agent_radius_offset = p_navigation_mesh->get_agent_radius();
	if (agent_radius_offset > 0.0) {
		path_solution = InflatePaths(path_solution, -agent_radius_offset, JoinType::Miter, EndType::Polygon);
	}

	if (!projected_obstructions.is_empty()) {
		obstruction_polygon_paths.resize(0);
		for (const NavigationMeshSourceGeometryData2D::ProjectedObstruction &projected_obstruction : projected_obstructions) {
			if (!projected_obstruction.carve) {
				continue;
			}
			if (projected_obstruction.vertices.is_empty() || projected_obstruction.vertices.size() % 2 != 0) {
				continue;
			}

			PathD clip_path;
			clip_path.reserve(projected_obstruction.vertices.size() / 2);
			for (int i = 0; i < projected_obstruction.vertices.size() / 2; i++) {
				const PointD &point = PointD(projected_obstruction.vertices[i * 2], projected_obstruction.vertices[i * 2 + 1]);
				clip_path.push_back(point);
			}
			if (!IsPositive(clip_path)) {
				std::reverse(clip_path.begin(), clip_path.end());
			}
			obstruction_polygon_paths.push_back(clip_path);
		}
		if (obstruction_polygon_paths.size() > 0) {
			path_solution = Difference(path_solution, obstruction_polygon_paths, FillRule::NonZero);
		}
	}

	//path_solution = RamerDouglasPeucker(path_solution, 0.025); //

	real_t border_size = p_navigation_mesh->get_border_size();
	if (baking_rect.has_area() && border_size > 0.0) {
		Vector2 baking_rect_offset = p_navigation_mesh->get_baking_rect_offset();

		const int rect_begin_x = baking_rect.position[0] + baking_rect_offset.x + border_size;
		const int rect_begin_y = baking_rect.position[1] + baking_rect_offset.y + border_size;
		const int rect_end_x = baking_rect.position[0] + baking_rect.size[0] + baking_rect_offset.x - border_size;
		const int rect_end_y = baking_rect.position[1] + baking_rect.size[1] + baking_rect_offset.y - border_size;

		RectD clipper_rect = RectD(rect_begin_x, rect_begin_y, rect_end_x, rect_end_y);

		path_solution = RectClip(clipper_rect, path_solution);
	}

	Vector<Vector<Vector2>> new_baked_outlines;

	for (const PathD &scaled_path : path_solution) {
		Vector<Vector2> polypath;
		for (const PointD &scaled_point : scaled_path) {
			polypath.push_back(Vector2(static_cast<real_t>(scaled_point.x), static_cast<real_t>(scaled_point.y)));
		}
		new_baked_outlines.push_back(polypath);
	}

	if (new_baked_outlines.size() == 0) {
		p_navigation_mesh->clear();
		return;
	}

	PathsD polygon_paths;
	polygon_paths.reserve(new_baked_outlines.size());

	for (const Vector<Vector2> &baked_outline : new_baked_outlines) {
		PathD polygon_path;
		for (const Vector2 &baked_outline_point : baked_outline) {
			const PointD &point = PointD(baked_outline_point.x, baked_outline_point.y);
			polygon_path.push_back(point);
		}
		polygon_paths.push_back(polygon_path);
	}

	ClipType clipper_cliptype = ClipType::Union;

	List<TPPLPoly> tppl_in_polygon, tppl_out_polygon;

	PolyTreeD polytree;
	ClipperD clipper_D;

	clipper_D.AddSubject(polygon_paths);
	clipper_D.Execute(clipper_cliptype, FillRule::NonZero, polytree);

	for (size_t i = 0; i < polytree.Count(); i++) {
		const PolyPathD *polypath_item = polytree[i];
		generator_recursive_process_polytree_items(tppl_in_polygon, polypath_item);
	}

	TPPLPartition tpart;
	if (tpart.ConvexPartition_HM(&tppl_in_polygon, &tppl_out_polygon) == 0) { //failed!
		ERR_PRINT("NavigationPolygon Convex partition failed. Unable to create a valid NavigationMesh from defined polygon outline paths.");
		p_navigation_mesh->clear();
		return;
	}

	Vector<Vector2> new_vertices;
	Vector<Vector<int>> new_polygons;

	HashMap<Vector2, int> points;
	for (List<TPPLPoly>::Element *I = tppl_out_polygon.front(); I; I = I->next()) {
		TPPLPoly &tp = I->get();

		Vector<int> new_polygon;

		for (int64_t i = 0; i < tp.GetNumPoints(); i++) {
			HashMap<Vector2, int>::Iterator E = points.find(tp[i]);
			if (!E) {
				E = points.insert(tp[i], new_vertices.size());
				new_vertices.push_back(tp[i]);
			}
			new_polygon.push_back(E->value);
		}

		new_polygons.push_back(new_polygon);
	}

	p_navigation_mesh->set_data(new_vertices, new_polygons);
}

#endif // CLIPPER2_ENABLED
