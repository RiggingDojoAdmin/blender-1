
#include "BLI_timeit.h"
#include "BLI_array_cxx.h"
#include "BLI_vector_adaptor.h"
#include "BLI_parallel.h"

#include "BKE_collision.h"
#include "BKE_modifier.h"

#include "FN_cpp_type.h"

#include "simulate.hpp"

namespace BParticles {

using BLI::ScopedVector;
using BLI::Vector;
using BLI::VectorAdaptor;
using FN::CPPType;

/************************************************
 * Collisions (taken from the old particle_system.c)
 *
 * The algorithm is roughly:
 *  1. Use a BVH tree to search for faces that a particle may collide with.
 *  2. Use Newton's method to find the exact time at which the collision occurs.
 *     https://en.wikipedia.org/wiki/Newton's_method
 *
 ************************************************/
#define COLLISION_MIN_RADIUS 0.001f     // TODO check if this is needed
#define COLLISION_MIN_DISTANCE 0.0001f  // TODO check if this is needed
#define COLLISION_ZERO 0.00001f

static void normal_from_closest_point_to_tri(
    float no[3], const float p[3], const float v0[3], const float v1[3], const float v2[3])
{
  // Calculate the normal using the closest point on the triangle. This makes sure that
  // particles can collide and be deflected in the correct direction when colliding with verts
  // or edges of the triangle.
  float point_on_tri[3];
  closest_on_tri_to_point_v3(point_on_tri, p, v0, v1, v2);
  sub_v3_v3v3(no, p, point_on_tri);
  normalize_v3(no);
}

static float distance_to_tri(float3 &p, std::array<float3, 3> &cur_tri_points, float radius)
{
  float3 closest_point;
  closest_on_tri_to_point_v3(
      closest_point, p, cur_tri_points[0], cur_tri_points[1], cur_tri_points[2]);

  return float3::distance(closest_point, p) - radius;
}

static void collision_interpolate_element(std::array<std::pair<float3, float3>, 3> &tri_points,
                                          std::array<float3, 3> &cur_tri_points,
                                          float t)
{
  for (int i = 0; i < tri_points.size(); i++) {
    cur_tri_points[i] = float3::interpolate(tri_points[i].first, tri_points[i].second, t);
  }
}

/* find first root in range [0-1] starting from 0 */
static float collision_newton_rhapson(std::pair<float3, float3> &particle_points,
                                      std::array<std::pair<float3, float3>, 3> &tri_points,
                                      float radius,
                                      float3 &coll_normal,
                                      float3 &hit_bary_weights,
                                      float3 &point_on_plane)
{
  std::array<float3, 3> cur_tri_points;
  float t0, t1, dt_init, d0, d1, dd;
  float3 p;

  dt_init = 0.001f;
  /* start from the beginning */
  t0 = 0.f;
  collision_interpolate_element(tri_points, cur_tri_points, t0);
  d0 = distance_to_tri(particle_points.first, cur_tri_points, radius);
  t1 = dt_init;
  d1 = 0.f;

  /* particle already inside face, so report collision */
  if (d0 <= COLLISION_ZERO) {
    p = particle_points.first;
    // Save barycentric weight for velocity calculation later
    interp_weights_tri_v3(
        hit_bary_weights, cur_tri_points[0], cur_tri_points[1], cur_tri_points[2], p);

    normal_from_closest_point_to_tri(
        coll_normal, p, cur_tri_points[0], cur_tri_points[1], cur_tri_points[2]);
    // TODO clean up
    float3 point = p;
    float3 normal = coll_normal;
    float3 p2;
    closest_on_tri_to_point_v3(p2, point, cur_tri_points[0], cur_tri_points[1], cur_tri_points[2]);
    float new_d = (p2 - point).length();
    if (new_d < radius + COLLISION_MIN_DISTANCE) {
      // printf("too close!\n");
      point_on_plane = p2 + normal * (radius + COLLISION_MIN_DISTANCE);
    }
    else {
      point_on_plane = p;
    }

    // printf("t = 0, d0 = %f\n", d0);

    // print_v3("p", p);
    // print_v3("first", particle_points.first);
    // print_v3("second", particle_points.second);
    // print_v3("point on plane", point_on_plane);

    return 0.f;
  }

  for (int iter = 0; iter < 10; iter++) {  //, itersum++) {
    // printf("\nt1 %f\n", t1);

    /* get current location */
    collision_interpolate_element(tri_points, cur_tri_points, t1);
    p = float3::interpolate(particle_points.first, particle_points.second, t1);

    d1 = distance_to_tri(p, cur_tri_points, radius);

    /* Zero gradient (no movement relative to element). Can't step from
     * here. */
    if (d1 == d0) {
      /* If first iteration, try from other end where the gradient may be
       * greater. Note: code duplicated below. */
      if (iter == 0) {
        t0 = 1.f;
        collision_interpolate_element(tri_points, cur_tri_points, t0);
        d0 = distance_to_tri(particle_points.second, cur_tri_points, radius);
        t1 = 1.0f - dt_init;
        d1 = 0.f;
        continue;
      }
      else {
        return -1.f;
      }
    }

    if (d1 <= COLLISION_ZERO) {
      if (t1 >= -COLLISION_ZERO && t1 <= 1.f) {
        // Save barycentric weight for velocity calculation later
        interp_weights_tri_v3(
            hit_bary_weights, cur_tri_points[0], cur_tri_points[1], cur_tri_points[2], p);

        normal_from_closest_point_to_tri(
            coll_normal, p, cur_tri_points[0], cur_tri_points[1], cur_tri_points[2]);

        // TODO clean up
        float3 point = p;
        float3 normal = coll_normal;
        float3 p2;
        closest_on_tri_to_point_v3(
            p2, point, cur_tri_points[0], cur_tri_points[1], cur_tri_points[2]);
        float new_d = (p2 - point).length();
        if (new_d < radius + COLLISION_MIN_DISTANCE) {
          // TODO should probably always do this
          // printf("too close!\n");
          point_on_plane = p2 + normal * (radius + COLLISION_MIN_DISTANCE);
        }
        else {
          point_on_plane = p;
        }

        // printf("old_d %f\n", new_d);
        // printf("new_d %f\n", (point_on_plane - p2).length());
        // print_v3("p", p);
        // print_v3("first", particle_points.first);
        // print_v3("second", particle_points.second);
        // print_v3("point on plane", point_on_plane);

        CLAMP(t1, 0.f, 1.f);
        return t1;
      }
      else {
        return -1.f;
      }
    }

    /* Derive next time step */
    dd = (t1 - t0) / (d1 - d0);

    t0 = t1;
    d0 = d1;

    t1 -= d1 * dd;

    /* Particle moving away from plane could also mean a strangely rotating
     * face, so check from end. Note: code duplicated above. */
    if (iter == 0 && t1 < 0.f) {
      t0 = 1.f;
      collision_interpolate_element(tri_points, cur_tri_points, t0);
      d0 = distance_to_tri(particle_points.second, cur_tri_points, radius);
      t1 = 1.0f - dt_init;
      d1 = 0.f;
      continue;
    }
    else if (iter == 1 && (t1 < -COLLISION_ZERO || t1 > 1.f)) {
      return -1.f;
    }
  }
  return -1.0;
}

typedef struct RayCastData {
  std::pair<float3, float3> particle_points;
  CollisionModifierData *collmd;
  float3 hit_vel;
  float duration;
  float start_time;
} RayCastData;

BLI_NOINLINE static void raycast_callback(void *userdata,
                                          int index,
                                          const BVHTreeRay *ray,
                                          BVHTreeRayHit *hit)
{
  RayCastData *rd = (RayCastData *)userdata;
  CollisionModifierData *collmd = rd->collmd;

  const MVertTri *vt = &collmd->tri[index];
  MVert *verts = collmd->x;
  const float *v0, *v1, *v2;
  float dist;

  v0 = verts[vt->tri[0]].co;
  v1 = verts[vt->tri[1]].co;
  v2 = verts[vt->tri[2]].co;

  if (collmd->is_static) {
    zero_v3(rd->hit_vel);

    if (ray->radius == 0.0f) {
      // TODO particles probably need to always have somekind of radius, so this can probably be
      // removed after testing is done.
      dist = bvhtree_ray_tri_intersection(ray, hit->dist, v0, v1, v2);
    }
    else {
      dist = bvhtree_sphereray_tri_intersection(ray, ray->radius, hit->dist, v0, v1, v2);
    }

    if (dist >= 0.0f && dist < hit->dist) {
      hit->index = index;
      hit->dist = dist;
      // Subract COLLISION_MIN_DISTANCE from the distance to make sure that we do not collide with
      // the exact same point if the particle does not have time to move away from the collision
      // point.
      madd_v3_v3v3fl(hit->co, ray->origin, ray->direction, dist - COLLISION_MIN_DISTANCE);

      normal_from_closest_point_to_tri(hit->no, hit->co, v0, v1, v2);
      // TODO clean up (unify this into a function and using it in the rapson code)
      float3 point = hit->co;
      float3 normal = hit->no;
      float3 p2;
      closest_on_tri_to_point_v3(p2, point, v0, v1, v2);
      float new_d = (p2 - point).length();
      if (new_d < ray->radius + COLLISION_MIN_DISTANCE) {
        // printf("too close!\n");
        point = p2 + normal * (ray->radius + COLLISION_MIN_DISTANCE);
        copy_v3_v3(hit->co, point);
      }
    }
    return;
  }

  MVert *new_verts = collmd->xnew;
  const float *v0_new, *v1_new, *v2_new;
  v0_new = new_verts[vt->tri[0]].co;
  v1_new = new_verts[vt->tri[1]].co;
  v2_new = new_verts[vt->tri[2]].co;

  std::array<std::pair<float3, float3>, 3> tri_points;
  float3 coll_normal;
  float3 hit_bary_weights;

  tri_points[0] = std::pair<float3, float3>(v0, v0_new);
  tri_points[1] = std::pair<float3, float3>(v1, v1_new);
  tri_points[2] = std::pair<float3, float3>(v2, v2_new);

  if (rd->start_time != 0.0f) {
    // Adjust the triangle start positions
    std::array<float3, 3> new_start_points;
    collision_interpolate_element(tri_points, new_start_points, rd->start_time);
    tri_points[0].first = new_start_points[0];
    tri_points[1].first = new_start_points[1];
    tri_points[2].first = new_start_points[2];
  }

  float3 point_on_plane;
  /* TODO this is to silence gcc "may be use un-intied" warnings. Look into if I missed a case
   * where this is actually true */
  zero_v3(point_on_plane);

  // Check if we get hit by the moving object
  float coll_time = collision_newton_rhapson(
      rd->particle_points, tri_points, ray->radius, coll_normal, hit_bary_weights, point_on_plane);

  dist = float3::distance(rd->particle_points.first, rd->particle_points.second) * coll_time;

  if (coll_time >= 0.f) {
    if (hit->index != -1 && dist >= 0.0f && dist >= hit->dist) {
      /* We have already collided with and other object at closer distance */
      return;
    }
    // We have a collision!
    hit->index = index;
    hit->dist = dist;

    // Calculate the velocity of the point we hit
    zero_v3(rd->hit_vel);
    for (int i = 0; i < 3; i++) {
      rd->hit_vel += (tri_points[i].second - tri_points[i].first) * hit_bary_weights[i] /
                     rd->duration;
    }

    // printf("====Best hit so far!\n");

    copy_v3_v3(hit->co, point_on_plane);
    copy_v3_v3(hit->no, coll_normal);
  }
}

BLI_NOINLINE static void simulate_particle_chunk(SimulationState &UNUSED(simulation_state),
                                                 ParticleAllocator &UNUSED(particle_allocator),
                                                 MutableAttributesRef attributes,
                                                 ParticleSystemInfo &system_info,
                                                 MutableArrayRef<float> remaining_durations,
                                                 float UNUSED(end_time),
                                                 ArrayRef<ColliderCache *> colliders)
{
  uint amount = attributes.size();
  BLI_assert(amount == remaining_durations.size());

  BufferCache buffer_cache;

  Array<float3> forces(attributes.size(), {0, 0, 0});
  for (Force *force : system_info.forces) {
    force->add_force(attributes, IndexRange(amount), buffer_cache, forces);
  }

  MutableArrayRef<float3> velocities = attributes.get<float3>("Velocity");
  MutableArrayRef<float3> positions = attributes.get<float3>("Position");
  MutableArrayRef<bool> dead_state = attributes.get<bool>("Dead");

  // system_info.collision_objects
  // simulation_state.m_depsgraph;
  // cloth_bvh_collision

  for (uint pindex : IndexRange(amount)) {
    // if (pindex != 84 /* && pindex != 357 && pindex != 378*/) {
    //  continue;
    //}

    float mass = 1.0f;
    float duration = remaining_durations[pindex];
    bool collided;
    int coll_num = 0;

    // Update the velocities here so that the potential distance traveled is correct in the
    // collision check.
    velocities[pindex] += duration * forces[pindex] * mass;
    // TODO check if there is issues with moving colliders and particles with 0 velocity.
    // (There is issues with particles with zero velocity as they will not collide with anything if
    // the ray lenght is 0.

    // Check if any 'collobjs' collide with the particles here
    if (colliders.size() != 0) {
      CollisionModifierData *prev_collider = NULL;
      int prev_hit_idx = -1;

      do {
        BVHTreeRayHit best_hit;
        float3 best_hit_vel;
        float max_move = (duration * velocities[pindex]).length();

        best_hit.dist = FLT_MAX;
        collided = false;

        for (ColliderCache *col : colliders) {
          CollisionModifierData *collmd = col->collmd;

          if (!collmd->bvhtree) {
            continue;
          }

          const int raycast_flag = BVH_RAYCAST_DEFAULT;

          BVHTreeRayHit hit;
          hit.index = -1;
          hit.dist = max_move;
          // TODO the particle radius seems a bit flaky with higher distances?
          float particle_radius = 0.05f;

          float3 start = positions[pindex];
          float3 dir = velocities[pindex].normalized();

          RayCastData rd;

          rd.collmd = collmd;
          rd.particle_points.first = start;
          rd.particle_points.second = start + duration * velocities[pindex];

          rd.duration = duration;
          rd.start_time = 1.0 - duration / remaining_durations[pindex];

          // TODO perhaps have two callbacks and check for static colider here instead?
          // So, if static use callback A otherwise B
          BLI_bvhtree_ray_cast_ex(collmd->bvhtree,
                                  start,
                                  dir,
                                  particle_radius,
                                  &hit,
                                  raycast_callback,
                                  &rd,
                                  raycast_flag);

          if (hit.index == -1 || best_hit.dist < hit.dist) {
            // We didn't hit anything
            continue;
          }
          if (prev_collider == collmd && prev_hit_idx == hit.index) {
            // We collided with the same face twice in a row.
            // Skip collision handling here as the set velocity from the previous collision
            // handling should keep the particle from tunneling through the face.
            continue;
          }

          best_hit = hit;
          best_hit_vel = rd.hit_vel;
          prev_collider = collmd;
          prev_hit_idx = hit.index;
          collided = true;
        }
        if (collided) {
          // dead_state[pindex] = true;

          float3 normal = best_hit.no;

          // printf("==== COLLIDED ====\n");
          // print_v3("best_hit", best_hit.co);

          // print_v3("hit normal", normal);
          // print_v3("hit velo", best_hit_vel);
          // print_v3("part velo", velocities[pindex]);

          // TODO now that we calculate the normal in a smooth way, we should not need to have a
          // "n_v" vector to get a correct normal vector for the bounce.
          float3 n_v = float3::project(velocities[pindex], normal);
          n_v = n_v.normalized();

          // Local velocity on the particle with the collider as reference point.
          // Note that we only take into account the velocity of the collider in the normal
          // direction as the other velocity will be negated again when moving back to the
          // global reference frame.
          float3 hit_normal_velo = float3::project(best_hit_vel, n_v);
          float3 local_velo = velocities[pindex] - hit_normal_velo;

          float dampening = 0.5f;
          // Add the dampening factor
          local_velo *= (1.0f - dampening);

          float normal_dot = dot_v3v3(n_v, local_velo) / hit_normal_velo.length();
          // printf("normal dot %f\n", normal_dot);
          if (normal_dot < 1.0f) {
            // We are not moving fast enough away from the collider. Make sure that we have at
            // least the same speed as it to avoid tunneling.
            // TODO Dampening messes this calulcation up as we assume that local_velo is just:
            // "velocities[pindex] - hit_normal_velo"
            // printf("trigger\n");
            local_velo -= (1.0 + normal_dot) * hit_normal_velo;
          }

          normal_dot = dot_v3v3(local_velo, n_v);

          // if (normal_dot > 0.0f) {
          //  normal_dot *= -1.0f;
          //}
          // if (normal_dot < 0.0f) {
          // The particle was moving into the collission plane
          float3 deflect_vel = local_velo - 2.0f * normal_dot * n_v;

          // print_v3("normal", normal);
          // printf("normal dir %f\n", normal_dot);
          // print_v3("n_v", n_v);
          // print_v3("vel hit", best_hit_vel);
          // print_v3("vel_pre", velocities[pindex]);
          // print_v3("deflect_vel", deflect_vel);

          if (dot_v3v3(deflect_vel, normal) < 0) {
            // TODO this is needed when a particle collides two times on an edge or vert...
            // Perhaps there is a way to solve this more elegantly?
            // This is bascially a safety check to see that we are actually moving away from the
            // collider and not further into it.
            deflect_vel *= -1.0f;
          }

          float3 temp;

          sub_v3_v3v3(temp, positions[pindex], best_hit.co);

          if (temp.length() > velocities[pindex].length()) {
            print_v3("best_hit", best_hit.co);
            printf("pindex: %d\n\n\n\n", pindex);
          }

          // float temp = (1.0f + dot_v3v3(deflect_vel, best_hit_vel)) / 2.0f;
          // TODO if particle is moving away from the plane, it assumes the velocity of the
          // collider
          positions[pindex] = best_hit.co;
          velocities[pindex] = deflect_vel;
          // print_v3("vel_post", velocities[pindex]);
          //}

          // TODO add a check here to see if we are inside the collider (current pos on the normal
          // is less than radius)

          // Calculate the remaining duration
          // printf("old dur: %f\n", duration);
          duration -= duration * (best_hit.dist / max_move);
          // printf("new dur: %f\n", duration);
          // float3 force_after_hit = velocities[pindex] + duration * forces[pindex] * mass;
          // if (dot_v3v3(force_after_hit, hit_normal_velo) / hit_normal_velo.length() > 1.0f)
          // {
          //  // Do not apply forces if it would make the particle tunnel through colliders
          // velocities[pindex] = force_after_hit;
          //}
          coll_num++;
        }
      } while (collided && coll_num < 10);
    }
    float3 move_vec = duration * velocities[pindex];
    positions[pindex] += move_vec;
  }
}

BLI_NOINLINE static void delete_tagged_particles_and_reorder(ParticleSet &particles)
{
  auto kill_states = particles.attributes().get<bool>("Dead");
  ScopedVector<uint> indices_to_delete;

  for (uint i : kill_states.index_range()) {
    if (kill_states[i]) {
      indices_to_delete.append(i);
    }
  }

  particles.destruct_and_reorder(indices_to_delete.as_ref());
}

BLI_NOINLINE static void simulate_particles_for_time_span(SimulationState &simulation_state,
                                                          ParticleAllocator &particle_allocator,
                                                          ParticleSystemInfo &system_info,
                                                          FloatInterval time_span,
                                                          MutableAttributesRef particle_attributes)
{
  // TODO check if we acutally have a collision node and take settings from that
  ListBase *coll_list = BKE_collider_cache_create(simulation_state.m_depsgraph, NULL, NULL);

  // Convert list to vector for speed, easier debugging, and type safety
  Vector<ColliderCache *> colliders(*coll_list, true);

  BLI::blocked_parallel_for(IndexRange(particle_attributes.size()), 1000, [&](IndexRange range) {
    Array<float> remaining_durations(range.size(), time_span.size());
    simulate_particle_chunk(simulation_state,
                            particle_allocator,
                            particle_attributes.slice(range),
                            system_info,
                            remaining_durations,
                            time_span.end(),
                            colliders);
  });

  BKE_collider_cache_free(&coll_list);
}

BLI_NOINLINE static void simulate_particles_from_birth_to_end_of_step(
    SimulationState &simulation_state,
    ParticleAllocator &particle_allocator,
    ParticleSystemInfo &system_info,
    float end_time,
    MutableAttributesRef particle_attributes)
{
  ArrayRef<float> all_birth_times = particle_attributes.get<float>("Birth Time");

  // TODO check if we acutally have a collision node and take settings from that
  ListBase *coll_list = BKE_collider_cache_create(simulation_state.m_depsgraph, NULL, NULL);

  // Convert list to vector for speed, easier debugging, and type safety
  Vector<ColliderCache *> colliders(*coll_list, true);

  BLI::blocked_parallel_for(IndexRange(particle_attributes.size()), 1000, [&](IndexRange range) {
    ArrayRef<float> birth_times = all_birth_times.slice(range);

    Array<float> remaining_durations(range.size());
    for (uint i : remaining_durations.index_range()) {
      remaining_durations[i] = end_time - birth_times[i];
    }

    simulate_particle_chunk(simulation_state,
                            particle_allocator,
                            particle_attributes.slice(range),
                            system_info,
                            remaining_durations,
                            end_time,
                            colliders);
  });
  BKE_collider_cache_free(&coll_list);
}

BLI_NOINLINE static void simulate_existing_particles(
    SimulationState &simulation_state,
    ParticleAllocator &particle_allocator,
    StringMap<ParticleSystemInfo> &systems_to_simulate)
{
  FloatInterval simulation_time_span = simulation_state.time().current_update_time();

  BLI::parallel_map_items(simulation_state.particles().particle_containers(),
                          [&](StringRef system_name, ParticleSet *particle_set) {
                            ParticleSystemInfo *system_info = systems_to_simulate.lookup_ptr(
                                system_name);
                            if (system_info == nullptr) {
                              return;
                            }

                            simulate_particles_for_time_span(simulation_state,
                                                             particle_allocator,
                                                             *system_info,
                                                             simulation_time_span,
                                                             particle_set->attributes());
                          });
}

BLI_NOINLINE static void create_particles_from_emitters(SimulationState &simulation_state,
                                                        ParticleAllocator &particle_allocator,
                                                        ArrayRef<Emitter *> emitters,
                                                        FloatInterval time_span)
{
  BLI::parallel_for(emitters.index_range(), [&](uint emitter_index) {
    Emitter &emitter = *emitters[emitter_index];
    EmitterInterface interface(simulation_state, particle_allocator, time_span);
    emitter.emit(interface);
  });
}

void simulate_particles(SimulationState &simulation_state,
                        ArrayRef<Emitter *> emitters,
                        StringMap<ParticleSystemInfo> &systems_to_simulate)
{
  // SCOPED_TIMER(__func__);

  // systems_to_simulate.foreach_item([](StringRef name, ParticleSystemInfo &system_info) {
  //  system_info.collision_objects.print_as_lines(
  //      name, [](const CollisionObject &collision_object) {
  //        std::cout << collision_object.object->id.name
  //                  << " - Damping: " << collision_object.damping << " - Location Old: "
  //                  << float3(collision_object.local_to_world_start.values[3])
  //                  << " - Location New: "
  //                  << float3(collision_object.local_to_world_end.values[3]);
  //      });
  //});

  ParticlesState &particles_state = simulation_state.particles();
  FloatInterval simulation_time_span = simulation_state.time().current_update_time();

  StringMultiMap<ParticleSet *> all_newly_created_particles;
  StringMultiMap<ParticleSet *> newly_created_particles;
  {
    ParticleAllocator particle_allocator(particles_state);
    BLI::parallel_invoke(
        [&]() {
          simulate_existing_particles(simulation_state, particle_allocator, systems_to_simulate);
        },
        [&]() {
          create_particles_from_emitters(
              simulation_state, particle_allocator, emitters, simulation_time_span);
        });

    newly_created_particles = particle_allocator.allocated_particles();
    all_newly_created_particles = newly_created_particles;
  }

  while (newly_created_particles.key_amount() > 0) {
    ParticleAllocator particle_allocator(particles_state);

    BLI::parallel_map_items(
        newly_created_particles, [&](StringRef name, ArrayRef<ParticleSet *> new_particle_sets) {
          ParticleSystemInfo *system_info = systems_to_simulate.lookup_ptr(name);
          if (system_info == nullptr) {
            return;
          }

          BLI::parallel_for(new_particle_sets.index_range(), [&](uint index) {
            ParticleSet &particle_set = *new_particle_sets[index];
            simulate_particles_from_birth_to_end_of_step(simulation_state,
                                                         particle_allocator,
                                                         *system_info,
                                                         simulation_time_span.end(),
                                                         particle_set.attributes());
          });
        });

    newly_created_particles = particle_allocator.allocated_particles();
    all_newly_created_particles.add_multiple(newly_created_particles);
  }

  BLI::parallel_map_items(all_newly_created_particles,
                          [&](StringRef name, ArrayRef<ParticleSet *> new_particle_sets) {
                            ParticleSet &main_set = particles_state.particle_container(name);

                            for (ParticleSet *set : new_particle_sets) {
                              main_set.add_particles(*set);
                              delete set;
                            }
                          });

  BLI::parallel_map_keys(systems_to_simulate, [&](StringRef name) {
    ParticleSet &particles = particles_state.particle_container(name);
    delete_tagged_particles_and_reorder(particles);
  });
}

}  // namespace BParticles
