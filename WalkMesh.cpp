#include "WalkMesh.hpp"

#include "read_write_chunk.hpp"

#include <glm/gtx/norm.hpp>
#include <glm/gtx/string_cast.hpp>

#include <iostream>
#include <fstream>
#include <algorithm>
#include <string>

using namespace std;

WalkMesh::WalkMesh(std::vector< glm::vec3 > const &vertices_, std::vector< glm::vec3 > const &normals_, std::vector< glm::uvec3 > const &triangles_)
	: vertices(vertices_), normals(normals_), triangles(triangles_) {

	//construct next_vertex map (maps each edge to the next vertex in the triangle):
	next_vertex.reserve(triangles.size()*3);
	auto do_next = [this](uint32_t a, uint32_t b, uint32_t c) {
		auto ret = next_vertex.insert(std::make_pair(glm::uvec2(a,b), c));
		assert(ret.second);
	};
	int count = 0;
	for (auto const &tri : triangles) {
		//cout << count << " " << tri.x << " " << tri.y << " " << tri.z << endl;
		count++;
		do_next(tri.x, tri.y, tri.z);
		do_next(tri.y, tri.z, tri.x);
		do_next(tri.z, tri.x, tri.y);
	}

	//DEBUG: are vertex normals consistent with geometric normals?
	for (auto const &tri : triangles) {
		glm::vec3 const &a = vertices[tri.x];
		glm::vec3 const &b = vertices[tri.y];
		glm::vec3 const &c = vertices[tri.z];
		glm::vec3 out = glm::normalize(glm::cross(b-a, c-a));

		float da = glm::dot(out, normals[tri.x]);
		float db = glm::dot(out, normals[tri.y]);
		float dc = glm::dot(out, normals[tri.z]);

		assert(da > 0.1f && db > 0.1f && dc > 0.1f);
	}
}

//project pt to the plane of triangle a,b,c and return the barycentric weights of the projected point:
glm::vec3 barycentric_weights(glm::vec3 const &a, glm::vec3 const &b, glm::vec3 const &c, glm::vec3 const &pt) {
	glm::vec3 norm = normalize(cross(b - a, c - b));
	glm::vec3 center = (a + b + c) / 3.0f;
	
	glm::vec3 proj = pt - norm * dot(norm, pt - center);
	
	float x = dot(norm, cross(b - a, proj - a));
	float y = dot(norm, cross(c - b, proj - b));
	float z = dot(norm, cross(a - c, proj - c));
	float sum = x + y + z;

	return glm::vec3(y/sum, z/sum, x/sum);
}

WalkPoint WalkMesh::nearest_walk_point(glm::vec3 const &world_point) const {
	assert(!triangles.empty() && "Cannot start on an empty walkmesh");

	WalkPoint closest;
	float closest_dis2 = std::numeric_limits< float >::infinity();

	for (auto const &tri : triangles) {
		//find closest point on triangle:

		glm::vec3 const &a = vertices[tri.x];
		glm::vec3 const &b = vertices[tri.y];
		glm::vec3 const &c = vertices[tri.z];

		//get barycentric coordinates of closest point in the plane of (a,b,c):
		glm::vec3 coords = barycentric_weights(a,b,c, world_point);

		//is that point inside the triangle?
		if (coords.x >= 0.0f && coords.y >= 0.0f && coords.z >= 0.0f) {
			//yes, point is inside triangle.
			float dis2 = glm::length2(world_point - to_world_point(WalkPoint(tri, coords)));
			if (dis2 < closest_dis2) {
				closest_dis2 = dis2;
				closest.indices = tri;
				closest.weights = coords;
			}
		} else {
			//check triangle vertices and edges:
			auto check_edge = [&world_point, &closest, &closest_dis2, this](uint32_t ai, uint32_t bi, uint32_t ci) {
				glm::vec3 const &a = vertices[ai];
				glm::vec3 const &b = vertices[bi];

				//find closest point on line segment ab:
				float along = glm::dot(world_point-a, b-a);
				float max = glm::dot(b-a, b-a);
				glm::vec3 pt;
				glm::vec3 coords;
				if (along < 0.0f) {
					pt = a;
					coords = glm::vec3(1.0f, 0.0f, 0.0f);
				} else if (along > max) {
					pt = b;
					coords = glm::vec3(0.0f, 1.0f, 0.0f);
				} else {
					float amt = along / max;
					pt = glm::mix(a, b, amt);
					coords = glm::vec3(1.0f - amt, amt, 0.0f);
				}

				float dis2 = glm::length2(world_point - pt);
				if (dis2 < closest_dis2) {
					closest_dis2 = dis2;
					closest.indices = glm::uvec3(ai, bi, ci);
					closest.weights = coords;
				}
			};
			check_edge(tri.x, tri.y, tri.z);
			check_edge(tri.y, tri.z, tri.x);
			check_edge(tri.z, tri.x, tri.y);
		}
	}
	assert(closest.indices.x < vertices.size());
	assert(closest.indices.y < vertices.size());
	assert(closest.indices.z < vertices.size());
	return closest;
}


void WalkMesh::walk_in_triangle(WalkPoint const &start, glm::vec3 const &step, WalkPoint *end_, float *time_) const {
	assert(end_);
	auto &end = *end_;

	assert(time_);
	auto &time = *time_;

	//Somewhat inspired by Thomas on Discord
	glm::vec3 const &a = vertices[start.indices.x];
	glm::vec3 const &b = vertices[start.indices.y];
	glm::vec3 const &c = vertices[start.indices.z];

	float a_weight = start.weights.x;
	float b_weight = start.weights.y;
	float c_weight = start.weights.z;
	//Handle edge case headache :'( 
	if(a_weight == 0)
		a_weight += 0.00001f;
	if(b_weight == 0)
		b_weight += 0.00001f;
	if(c_weight == 0)
		c_weight += 0.00001f;

	//transform 'step' into a barycentric velocity on (a,b,c)
	glm::vec3 end_weights = barycentric_weights(a, b, c, to_world_point(start) + step);
	glm::vec3 b_vel = end_weights - start.weights;
	
	float ta = a_weight / -b_vel.x;
	float tb = b_weight / -b_vel.y;
	float tc = c_weight / -b_vel.z;
	end = start;
	
	//check when/if this velocity pushes start.weights into an edge
	int min = -1;
	std::vector<float> sorted_time;
	
	if(ta >= 0 && ta < 1)
		sorted_time.emplace_back(ta);
	if(tb >= 0 && tb < 1)
		sorted_time.emplace_back(tb);
	if (tc >= 0 && tc < 1)
		sorted_time.emplace_back(tc);
	
	std::sort(sorted_time.begin(), sorted_time.end());
	
	if (sorted_time.size() == 0) {
		min = -1;
	}
	else if (sorted_time[0] == ta) {
		min = 0;
	}
	else if (sorted_time[0] == tb) {
		min = 1;
	}
	else if (sorted_time[0] == tc) {
		min = 2;
	}
	
	if(min == 0) {
		glm::vec3 new_weights =  start.weights + b_vel * ta;
		end.indices = glm::uvec3(start.indices[1], start.indices[2], start.indices[0]);
		end.weights = glm::vec3(new_weights[1], new_weights[2], 0);
		time = ta;
	}
	else if(min == 1) {
		glm::vec3 new_weights =  start.weights + b_vel * tb;
		end.indices = glm::uvec3(start.indices[2], start.indices[0], start.indices[1]);
		end.weights = glm::vec3(new_weights[2], new_weights[0], 0);
		time = tb;
	}
	else if(min == 2) {
		end.indices = start.indices;
		end.weights = start.weights + b_vel * tc;
		end.weights[2] = 0;
		time = tc;	
	}
	else {
		end.indices = start.indices;
		end.weights = end_weights;
		time = 1;
	}

	// cout << "walk tri" << endl;

	// cout << "START WEIGHTS " << start.weights[0] << " " << start.weights[1] << " " << start.weights[2] << std::endl;
	// cout << "END WEIGHTS " << end_weights[0] << " " << end_weights[1] << " " << end_weights[2] << std::endl;
	// cout << "TIME " << ta << " " << tb << " " << tc << std::endl;
	// cout << "VEL " << b_vel[0] << " " << b_vel[1] << " " << b_vel[2] << std::endl;
	// cout << "MIN " << min << endl;
	// cout << "final weights " << end.weights.x << " " <<  end.weights.y << " " <<  end.weights.z << endl;
	// cout << "final indices " << end.indices.x << " " <<  end.indices.y << " " <<  end.indices.z << endl;
	// cout << "end walk tri" << endl;
}

bool WalkMesh::cross_edge(
	WalkPoint const &start, //[in] walkpoint on triangle edge
	WalkPoint *end_,         //[out] end walkpoint, having crossed edge
	glm::quat *rotation_,     //[out] rotation over edge
	std::unordered_map< glm::uvec2, std::vector<uint32_t> > *walked_on_
) const {
	assert(end_);
	auto &end = *end_;

	assert(rotation_);
	auto &rotation = *rotation_;

	auto &walked_on = *walked_on_;

	assert(start.weights.z == 0.0f); //*must* be on an edge.


	// check if edge (start.indices.x, start.indices.y) has a triangle on the other side:
	//cout << "Attempting to cross edge " << start.indices.x << " " << start.indices.y << endl;

	glm::uvec2 edge = glm::uvec2(start.indices.y, start.indices.x);
	auto next_edge = next_vertex.find(edge);
	if(next_edge == next_vertex.end())
		return false;
	
	//cout << vertices.size() << endl;
	cout << "Crossing from " << start.indices.x << " " << start.indices.y << " " << start.indices.z << " to " << start.indices.y << " " << start.indices.x << " " << (int)next_edge->second << endl;
	auto min_med_max = [](uint32_t a, uint32_t b, uint32_t c) {
		if(min(a, min(b, c)) == a) {
			return glm::uvec3(a, min(b, c), max(b, c));
		}
		else if(min(a, min(b, c)) == b) {
			return glm::uvec3(b, min(a, c), max(a, c));
		}
		else {
			return glm::uvec3(c, min(a, b), max(a, b));
		}
	};

	auto next_tri = min_med_max(start.indices.y, start.indices.x, next_edge->second);
	cout << "Sorted next tri " << next_tri.x << " " << next_tri.y << " " << next_tri.z << endl;
	auto to_find = glm::uvec2(next_tri.x, next_tri.y);
	auto it = walked_on.find(to_find);
	bool found = false;

	//This edge pair is found
	if(it != walked_on.end()) {
		cout << "found" << endl;
		//If the triangle is already walked on, remove it
		vector< uint32_t > new_vec;
		for(auto &third_vertex : it->second) {
			cout << third_vertex << " " << next_tri.z << endl;
			if(third_vertex != next_tri.z) {
				new_vec.emplace_back(third_vertex);
			}
			else {
				found = true;
			}
		}

		//Found, but the triangle is not already walked on
		if(found == false) {
			new_vec.emplace_back(next_tri.z);
		}

		walked_on.erase(to_find);
		walked_on[to_find] = new_vec;
	}
	//Didn't find it, create vector and insert
	else {
		std::vector<uint32_t> vec;
		vec.emplace_back(next_tri.z);
		auto ret = walked_on.insert(std::make_pair(glm::uvec2(next_tri.x, next_tri.y), vec));
		assert(ret.second);
	}

	// if there is another triangle:
	// set end's weights and indicies on that triangle:
	end.indices = glm::vec3(start.indices.y, start.indices.x, next_edge->second);
	end.weights = glm::vec3(start.weights.y, start.weights.x, 0);

	// compute rotation that takes starting triangle's normal to ending triangle's normal:
	glm::vec3 const &b = vertices[start.indices.x];
	glm::vec3 const &c = vertices[start.indices.y];
	glm::vec3 const &a = vertices[start.indices.z];
	glm::vec3 const &d = vertices[next_edge->second];

	glm::vec3 start_norm = normalize(cross(c - b, a - b));
	glm::vec3 end_norm = normalize(cross(d - b, c - b));
	
	rotation = glm::rotation(start_norm, end_norm);

	//return 'true' if there was another triangle, 'false' otherwise:
	return true;
}


WalkMeshes::WalkMeshes(std::string const &filename) {
	std::ifstream file(filename, std::ios::binary);

	std::vector< glm::vec3 > vertices;
	read_chunk(file, "p...", &vertices);

	std::vector< glm::vec3 > normals;
	read_chunk(file, "n...", &normals);

	std::vector< glm::uvec3 > triangles;
	read_chunk(file, "tri0", &triangles);

	std::vector< char > names;
	read_chunk(file, "str0", &names);

	struct IndexEntry {
		uint32_t name_begin, name_end;
		uint32_t vertex_begin, vertex_end;
		uint32_t triangle_begin, triangle_end;
	};

	std::vector< IndexEntry > index;
	read_chunk(file, "idxA", &index);

	if (file.peek() != EOF) {
		std::cerr << "WARNING: trailing data in walkmesh file '" << filename << "'" << std::endl;
	}

	//-----------------

	if (vertices.size() != normals.size()) {
		throw std::runtime_error("Mis-matched position and normal sizes in '" + filename + "'");
	}

	for (auto const &e : index) {
		if (!(e.name_begin <= e.name_end && e.name_end <= names.size())) {
			throw std::runtime_error("Invalid name indices in index of '" + filename + "'");
		}
		if (!(e.vertex_begin <= e.vertex_end && e.vertex_end <= vertices.size())) {
			throw std::runtime_error("Invalid vertex indices in index of '" + filename + "'");
		}
		if (!(e.triangle_begin <= e.triangle_end && e.triangle_end <= triangles.size())) {
			throw std::runtime_error("Invalid triangle indices in index of '" + filename + "'");
		}

		//copy vertices/normals:
		std::vector< glm::vec3 > wm_vertices(vertices.begin() + e.vertex_begin, vertices.begin() + e.vertex_end);
		std::vector< glm::vec3 > wm_normals(normals.begin() + e.vertex_begin, normals.begin() + e.vertex_end);

		//remap triangles:
		std::vector< glm::uvec3 > wm_triangles; wm_triangles.reserve(e.triangle_end - e.triangle_begin);
		for (uint32_t ti = e.triangle_begin; ti != e.triangle_end; ++ti) {
			if (!( (e.vertex_begin <= triangles[ti].x && triangles[ti].x < e.vertex_end)
			    && (e.vertex_begin <= triangles[ti].y && triangles[ti].y < e.vertex_end)
			    && (e.vertex_begin <= triangles[ti].z && triangles[ti].z < e.vertex_end) )) {
				throw std::runtime_error("Invalid triangle in '" + filename + "'");
			}
			wm_triangles.emplace_back(
				triangles[ti].x - e.vertex_begin,
				triangles[ti].y - e.vertex_begin,
				triangles[ti].z - e.vertex_begin
			);
		}
		
		std::string name(names.begin() + e.name_begin, names.begin() + e.name_end);

		auto ret = meshes.emplace(name, WalkMesh(wm_vertices, wm_normals, wm_triangles));
		if (!ret.second) {
			throw std::runtime_error("WalkMesh with duplicated name '" + name + "' in '" + filename + "'");
		}

	}
}

WalkMesh const &WalkMeshes::lookup(std::string const &name) const {
	auto f = meshes.find(name);
	if (f == meshes.end()) {
		throw std::runtime_error("WalkMesh with name '" + name + "' not found.");
	}
	return f->second;
}
