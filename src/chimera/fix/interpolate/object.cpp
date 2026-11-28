// SPDX-License-Identifier: GPL-3.0-only

#include <cmath>
#include <chrono>
#include <algorithm>

#include "../../signature/signature.hpp"
#include "../../halo_data/object.hpp"
#include "../../halo_data/player.hpp"
#include "../../math_trig/math_trig.hpp"
#include "../../chimera.hpp"

#include "interpolate.hpp"

#include "object.hpp"

namespace Chimera {
    #define OBJECT_BUFFER_SIZE 2048
    struct InterpolatedObject {
        /** Interpolate this object. */
        bool interpolate = false;

        /** This object was interpolated and needs to be uninterpolated. This is so we don't need to do so many checks twice. */
        bool interpolated_this_frame = false;

        /** This is the number of objects parented to this object. */
        std::size_t children_count = 0;

        /** These are the children that parent this object. */
        std::size_t children[OBJECT_BUFFER_SIZE];

        /** Tag ID of the object. */
        TagID tag_id;

        /** Parent object captured with this tick (vehicle when occupied). */
        ObjectID parent = HaloID::null_id();

        /** This biped left a parented vehicle during this tick. */
        bool vehicle_exit = false;

        /** This is the position of the object's center. */
        Point3D center;

        /** This is the number of nodes this object has. */
        std::size_t node_count;

        /** These are the model nodes used by the object. */
        ModelNode nodes[MAX_NODES];
    };

    // This is the object data to interpolate.
    static InterpolatedObject object_buffers[2][OBJECT_BUFFER_SIZE] = {};

    // These are pointers to each buffer. These swap every tick.
    static auto *current_tick = object_buffers[0];
    static auto *previous_tick = object_buffers[1];

    // If true, a tick has passed and it's time to re-copy the FP data.
    static bool tick_passed = false;

    // Smooth extrapolation: how far ahead of the interpolated position to
    // draw, in ticks (0.5 = half a tick ahead). The drawn offset is eased
    // toward its target every rendered frame, so the rendered position stays
    // continuous across tick boundaries and direction changes, exactly like
    // plain interpolation, instead of snapping like raw extrapolation.
    static const float SMOOTH_EXTRAPOLATION_LEAD = 0.5f;

    // Time constant for exponential smoothing. Unlike a fixed per-frame
    // factor, this produces the same response at different render rates.
    static const float SMOOTH_EXTRAPOLATION_SMOOTHING_RATE = 18.0f;
    static float frame_delta_seconds = 1.0f / 60.0f;
    static std::chrono::steady_clock::time_point previous_frame_time;
    static bool has_previous_frame_time = false;

    // After a sharp direction change (angle between this tick's velocity and
    // the previous tick's velocity greater than ~60 degrees), the target
    // offset is scaled down to this fraction until the object moves steadily
    // again, so the drawn position never chases a wrong prediction.
    static const float SMOOTH_EXTRAPOLATION_TURN_FACTOR = 0.25f;
    static const float SMOOTH_EXTRAPOLATION_TURN_COS = 0.5f;

    /**
     * Per-object smooth-extrapolation state, indexed by object table index.
     * It lives outside the tick buffers so it survives buffer swaps and
     * keeps the drawn offset continuous from frame to frame.
     */
    struct ExtrapolationState {
        /** Extra offset currently being drawn (world units). */
        Point3D smoothed_extra = {};
        /** Per-tick displacement of the previous tick. */
        Point3D last_velocity = {};
        /** 1.0 while moving steadily, 0.25 right after a sharp direction change. */
        float direction_factor = 1.0f;
        /** True once two consecutive ticks have been observed. */
        bool has_velocity = false;
    };
    static ExtrapolationState extrapolation_state[OBJECT_BUFFER_SIZE] = {};

    static void copy_objects() noexcept;
    static void interpolate_object(std::size_t);

    static void update_frame_delta() noexcept {
        auto now = std::chrono::steady_clock::now();
        if(has_previous_frame_time) {
            frame_delta_seconds = std::chrono::duration<float>(now - previous_frame_time).count();
            frame_delta_seconds = std::max(0.0f, std::min(frame_delta_seconds, 0.1f));
        }
        previous_frame_time = now;
        has_previous_frame_time = true;
    }

    // Scale prediction from no lead at 40 ms to the legacy half-tick lead
    // at 80 ms. If player ownership/ping is unavailable, preserve legacy
    // behavior rather than unexpectedly disabling extrapolation.
    static float get_extrapolation_lead(const BaseDynamicObject &object) noexcept {
        if(object.type != OBJECT_TYPE_BIPED && object.type != OBJECT_TYPE_VEHICLE) {
            return SMOOTH_EXTRAPOLATION_LEAD;
        }
        auto *unit = reinterpret_cast<const UnitDynamicObject *>(&object);
        if(unit->player_id.is_null()) {
            return SMOOTH_EXTRAPOLATION_LEAD;
        }
        auto *player = PlayerTable::get_player_table().get_player(unit->player_id);
        if(!player || player->ping == 0) {
            return SMOOTH_EXTRAPOLATION_LEAD;
        }
        if(player->ping <= 40) {
            return 0.0f;
        }
        if(player->ping >= 80) {
            return SMOOTH_EXTRAPOLATION_LEAD;
        }
        return SMOOTH_EXTRAPOLATION_LEAD * (static_cast<float>(player->ping - 40) / 40.0f);
    }

    // Ease one object's drawn extra offset toward its target for this frame.
    // The target is half a tick of travel in the object's current direction
    // (reduced right after a sharp direction change), clamped so a sudden
    // change can never throw the object far from its real position. Because
    // the offset is (nearly) constant within a tick and only eased between
    // frames, the drawn object moves at real speed and never snaps, just like
    // plain interpolation — only shifted slightly ahead.
    static void update_smoothed_extra(const Point3D &before, const Point3D &after, ExtrapolationState &state, float max_extra, float lead) noexcept {
        float tx = (after.x - before.x) * lead * state.direction_factor;
        float ty = (after.y - before.y) * lead * state.direction_factor;
        float tz = (after.z - before.z) * lead * state.direction_factor;
        float target_sq = tx*tx + ty*ty + tz*tz;
        float max_extra_sq = max_extra * max_extra;
        if(target_sq > max_extra_sq && target_sq > 0.0f) {
            float s = std::sqrt(max_extra_sq / target_sq);
            tx *= s; ty *= s; tz *= s;
        }
        float smoothing = 1.0f - std::exp(-SMOOTH_EXTRAPOLATION_SMOOTHING_RATE * frame_delta_seconds);
        state.smoothed_extra.x += (tx - state.smoothed_extra.x) * smoothing;
        state.smoothed_extra.y += (ty - state.smoothed_extra.y) * smoothing;
        state.smoothed_extra.z += (tz - state.smoothed_extra.z) * smoothing;
    }

    void interpolate_object_before() noexcept {
        update_frame_delta();

        // Check if a tick has passed. If so, swap buffers and copy new objects.
        if(tick_passed) {
            if(current_tick == object_buffers[0]) {
                current_tick = object_buffers[1];
                previous_tick = object_buffers[0];
            }
            else {
                current_tick = object_buffers[0];
                previous_tick = object_buffers[1];
            }

            copy_objects();
            tick_passed = false;
        }

        static auto **visible_object_count = reinterpret_cast<std::uint32_t **>(get_chimera().get_signature("visible_object_count_sig").data() + 3);
        static auto **visible_object_array = reinterpret_cast<ObjectID **>(get_chimera().get_signature("visible_object_ptr_sig").data() + 3);
        auto current_count = **visible_object_count;

        for(std::size_t i = 0; i < current_count; i++) {
            interpolate_object((*visible_object_array)[i].index.index);
        }
    }

    static void interpolate_object(std::size_t index) {
        extern float interpolation_tick_progress;

        // Don't interpolate out-of-bounds indices
        if(index >= OBJECT_BUFFER_SIZE) {
            return;
        }

        auto &current_tick_object = current_tick[index];
        auto &previous_tick_object = previous_tick[index];

        // Skip objects we can't interpolate or were already interpolated.
        if(!current_tick_object.interpolate || !previous_tick_object.interpolate || current_tick_object.interpolated_this_frame || previous_tick_object.interpolated_this_frame) {
            return;
        }

        auto *object = ObjectTable::get_object_table().get_dynamic_object(index);

        // If for some reason the object ID is invalid, skip that too.
        if(!object) {
            return;
        }

        // Skip if the tags do not match
        auto &tag_id = object->tag_id;
        if(tag_id != current_tick_object.tag_id || previous_tick_object.tag_id != tag_id) {
            return;
        }

        // Skip if the node counts don't match
        if(previous_tick_object.node_count != current_tick_object.node_count) {
            return;
        }

        // Set this flag so we don't need to do all these checks again when rolling things back.
        current_tick_object.interpolated_this_frame = true;

        // Search for all objects that parent this object.
        for(std::size_t i = 0; i < current_tick_object.children_count; i++) {
            interpolate_object(current_tick_object.children[i]);
        }

        // Interpolate the center thingymajigabobit. Bipeds and vehicles are
        // extrapolated half a tick ahead with a smoothed offset (when
        // enabled), so the drawn enemy is close to where it will be when the
        // next tick simulates it, but the motion stays as fluid as plain
        // interpolation. Everything else keeps the plain interpolation.
        bool extrapolate = interpolate_extrapolate && (object->type == OBJECT_TYPE_BIPED || object->type == OBJECT_TYPE_VEHICLE);
        float max_extra = (object->type == OBJECT_TYPE_BIPED) ? 2.5f : 7.5f;
        float extrapolation_lead = get_extrapolation_lead(*object);
        auto &extrapolation = extrapolation_state[index];
        interpolate_point(previous_tick_object.center, current_tick_object.center, object->center_position, interpolation_tick_progress);
        if(extrapolate) {
            // One smoothing step per rendered frame, shared by the center and
            // all nodes so the whole body moves as one, without shearing.
            update_smoothed_extra(previous_tick_object.center, current_tick_object.center, extrapolation, max_extra, extrapolation_lead);
            object->center_position.x += extrapolation.smoothed_extra.x;
            object->center_position.y += extrapolation.smoothed_extra.y;
            object->center_position.z += extrapolation.smoothed_extra.z;
        }
        else {
            // While extrapolation is off, drop the stored offset so turning
            // it back on cannot jump the object.
            extrapolation.smoothed_extra = {};
        }

        auto *nodes = object->nodes();

        for(std::size_t n = 0; n < current_tick_object.node_count; n++) {
            auto &node = nodes[n];
            auto &node_current = current_tick_object.nodes[n];
            auto &node_before = previous_tick_object.nodes[n];

            // Interpolate position. Bipeds/vehicles also get the same
            // smoothed extra offset as the center, so the whole body stays
            // coherent and fluid.
            interpolate_point(node_before.position, node_current.position, node.position, interpolation_tick_progress);
            if(extrapolate) {
                node.position.x += extrapolation.smoothed_extra.x;
                node.position.y += extrapolation.smoothed_extra.y;
                node.position.z += extrapolation.smoothed_extra.z;
            }

            // Interpolate scale
            node.scale = node_before.scale + (node_current.scale - node_before.scale) * interpolation_tick_progress;

            // Interpolate it all!
            Quaternion orientation_current = node_current.rotation;
            Quaternion orientation_before = node_before.rotation;
            Quaternion orientation_interpolated;
            interpolate_quat(orientation_before, orientation_current, orientation_interpolated, interpolation_tick_progress);
            node.rotation = orientation_interpolated;
        }
    }

    // Copy objects from Halo's data to buffer
    static void copy_objects() noexcept {
        // Get the object table
        auto &object_table = ObjectTable::get_object_table();

        // Go through all objects.
        auto max_size = object_table.current_size;
        for(std::size_t i = 0; i < OBJECT_BUFFER_SIZE; i++) {
            auto &current_tick_object = current_tick[i];
            current_tick_object.interpolated_this_frame = false;

            // Set this to false so if it doesn't exist or we can't interpolate it for some reason, we don't have to worry about it.
            current_tick_object.interpolate = false;

            // See if the object exists.
            auto *object = object_table.get_dynamic_object(i);
            if(!object) {
                continue;
            }

            // Check if the object isn't visible.
            bool is_weapon = object->type == ObjectType::OBJECT_TYPE_WEAPON;
            if(object->no_collision && is_weapon) {
                continue;
            }

            // Check if we can actually interpolate this object.
            auto *nodes = object->nodes();
            if(!nodes) {
                continue;
            }

            // Get the number of model nodes.
            current_tick_object.tag_id = object->tag_id;
            current_tick_object.parent = object->parent;
            current_tick_object.vehicle_exit = false;
            auto *object_tag = get_tag(current_tick_object.tag_id.index.index);
            if(!object_tag) {
                continue;
            }

            // Get the model tag to get the node count
            if(object->type == ObjectType::OBJECT_TYPE_PROJECTILE) {
                current_tick_object.node_count = 1;
            }
            else {
                const auto &model_tag_id = *reinterpret_cast<const TagID *>(object_tag->data + 0x28 + 0xC);
                auto *model_tag = get_tag(model_tag_id);
                if(!model_tag) {
                    current_tick_object.node_count = 0;
                }
                else {
                    current_tick_object.node_count = *reinterpret_cast<std::uint32_t *>(model_tag->data + 0xB8);
                }
            }

            // Copy nodes from Halo's data
            std::copy(nodes, nodes + current_tick_object.node_count, current_tick_object.nodes);
            current_tick_object.center = object->center_position;

            // Normal biped movement is limited to 2.5 world units per tick.
            // When a biped exits a moving vehicle, however, its world-space
            // displacement can include the vehicle's travel. Account for
            // that one transition only, with a hard cap so real teleports
            // are still rejected.
            float max_interpolation_distance = (object->type == OBJECT_TYPE_BIPED) ? 2.5f : 7.5f;
            bool vehicle_exit = object->type == OBJECT_TYPE_BIPED &&
                                !previous_tick[i].parent.is_null() &&
                                current_tick_object.parent.is_null();
            if(vehicle_exit) {
                auto parent_index = previous_tick[i].parent.index.index;
                if(parent_index < OBJECT_BUFFER_SIZE) {
                    const auto &previous_vehicle = previous_tick[parent_index];
                    // Read the live parent directly instead of relying on
                    // current_tick[parent_index], whose copy order may not
                    // have reached the vehicle yet.
                    auto *vehicle = object_table.get_dynamic_object(previous_tick[i].parent);
                    if(previous_vehicle.interpolate && vehicle &&
                       vehicle->type == OBJECT_TYPE_VEHICLE &&
                       previous_vehicle.tag_id == vehicle->tag_id) {
                        float vehicle_dx = vehicle->center_position.x - previous_vehicle.center.x;
                        float vehicle_dy = vehicle->center_position.y - previous_vehicle.center.y;
                        float vehicle_dz = vehicle->center_position.z - previous_vehicle.center.z;
                        float vehicle_travel = std::sqrt(vehicle_dx * vehicle_dx + vehicle_dy * vehicle_dy + vehicle_dz * vehicle_dz);
                        max_interpolation_distance += std::min(vehicle_travel, 12.5f);
                    }
                }
            }
            current_tick_object.vehicle_exit = vehicle_exit;
            float max_interpolation_distance_sq = max_interpolation_distance * max_interpolation_distance;

            // A large unexplained jump (respawn/teleport/object reuse) still
            // fails this test; only a confirmed vehicle-exit transition gets
            // the bounded vehicle-motion allowance above.
            current_tick_object.interpolate = distance_squared(current_tick_object.center, previous_tick[i].center) < max_interpolation_distance_sq;

            // Smooth extrapolation bookkeeping, once per tick: track this
            // tick's velocity and spot sharp direction changes.
            auto &extrapolation = extrapolation_state[i];
            if(current_tick_object.interpolate && current_tick_object.tag_id == previous_tick[i].tag_id) {
                Point3D velocity = {
                    current_tick_object.center.x - previous_tick[i].center.x,
                    current_tick_object.center.y - previous_tick[i].center.y,
                    current_tick_object.center.z - previous_tick[i].center.z
                };
                if(extrapolation.has_velocity) {
                    float last_len = std::sqrt(extrapolation.last_velocity.x * extrapolation.last_velocity.x + extrapolation.last_velocity.y * extrapolation.last_velocity.y + extrapolation.last_velocity.z * extrapolation.last_velocity.z);
                    float vel_len = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
                    if(last_len > 0.001f && vel_len > 0.001f) {
                        float cos_angle = (extrapolation.last_velocity.x * velocity.x + extrapolation.last_velocity.y * velocity.y + extrapolation.last_velocity.z * velocity.z) / (last_len * vel_len);
                        extrapolation.direction_factor = (cos_angle < SMOOTH_EXTRAPOLATION_TURN_COS) ? SMOOTH_EXTRAPOLATION_TURN_FACTOR : 1.0f;
                    }
                    else {
                        extrapolation.direction_factor = 1.0f;
                    }
                }
                extrapolation.last_velocity = velocity;
                extrapolation.has_velocity = true;
                if(current_tick_object.vehicle_exit) {
                    // The first sample after leaving a vehicle may contain
                    // inherited vehicle momentum; be conservative for this tick.
                    extrapolation.direction_factor = SMOOTH_EXTRAPOLATION_TURN_FACTOR;
                }
            }
            else {
                // Not interpolated (teleported, despawned, or a different
                // object reusing this slot): drop stale smoothing state.
                extrapolation = ExtrapolationState();
            }

            // Get any children of this object
            current_tick_object.children_count = 0;
            if(current_tick_object.interpolate) {
                for(std::size_t o = 0; o < max_size && o < OBJECT_BUFFER_SIZE; o++) {
                    auto *object = object_table.get_dynamic_object(o);
                    if(object && object->parent.index.index == i) {
                        current_tick_object.children[current_tick_object.children_count++] = o;
                    }
                }
            }
        }
    }

    void interpolate_object_after() noexcept {
        auto &object_table = ObjectTable::get_object_table();
        auto max_objects = object_table.current_size;
        for(std::size_t i = 0; i < max_objects && i < OBJECT_BUFFER_SIZE; i++) {
            auto &current_tick_object = current_tick[i];

            // Skip if we didn't interpolate this frame.
            if(!current_tick_object.interpolated_this_frame) {
                continue;
            }

            // Unset so we can interpolate again next frame
            current_tick_object.interpolated_this_frame = false;

            auto *object = object_table.get_dynamic_object(i);

            // This shouldn't ever happen but just in case it does...
            if(!object) {
                continue;
            }

            object->center_position = current_tick_object.center;
            std::copy(current_tick_object.nodes, current_tick_object.nodes + current_tick_object.node_count, object->nodes());
        }
    }

    void interpolate_object_clear() noexcept {
        for(std::size_t i = 0; i < OBJECT_BUFFER_SIZE; i++) {
            current_tick[i].interpolate = false;
            previous_tick[i].interpolate = false;
            current_tick[i].interpolated_this_frame = false;
            previous_tick[i].interpolated_this_frame = false;
            extrapolation_state[i] = ExtrapolationState();
        }
    }

    void interpolate_object_on_tick() noexcept {
        tick_passed = true;
    }
}
