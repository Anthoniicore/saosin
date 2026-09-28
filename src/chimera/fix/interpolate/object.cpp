// SPDX-License-Identifier: GPL-3.0-only

#include "../../signature/signature.hpp"
#include "../../halo_data/object.hpp"
#include "../../math_trig/math_trig.hpp"
#include "../../chimera.hpp"
#include <cmath>

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

    static void copy_objects() noexcept;
    static void interpolate_object(std::size_t);

    void interpolate_object_before() noexcept {
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

        auto *nodes = object->nodes();
        if(!nodes) {
            return;
        }

        // EXTRAPOLATION mode is deliberately visual-only. The authoritative
        // Halo object remains on the current tick and is restored in
        // interpolate_object_after().
        //
        // Only unparented bipeds use this path. This avoids touching vehicles
        // or child-object transforms where parent-relative coordinates could
        // otherwise make prediction unsafe.
        if(get_interpolation_mode() == InterpolationMode::EXTRAPOLATION &&
           object->type == ObjectType::OBJECT_TYPE_BIPED &&
           object->parent.is_null()) {
            Point3D delta {
                object->velocity.x * interpolation_tick_progress,
                object->velocity.y * interpolation_tick_progress,
                object->velocity.z * interpolation_tick_progress
            };

            // Keep the prediction within the same per-tick movement envelope
            // already used by the traditional biped interpolation safety gate.
            constexpr float MAX_BIPED_EXTRAPOLATION = 2.5F;
            float delta_distance_squared = distance_squared(delta.x, delta.y, delta.z, 0.0F, 0.0F, 0.0F);
            float max_delta = MAX_BIPED_EXTRAPOLATION * interpolation_tick_progress;
            if(delta_distance_squared > max_delta * max_delta) {
                float delta_distance = std::sqrt(delta_distance_squared);
                if(delta_distance > 0.0F) {
                    float scale = max_delta / delta_distance;
                    delta.x *= scale;
                    delta.y *= scale;
                    delta.z *= scale;
                }
            }

            // Translate the current-tick geometry forward by only the
            // fraction of the tick that has elapsed. No previous-tick
            // position is rendered in this mode.
            object->center_position.x = current_tick_object.center.x + delta.x;
            object->center_position.y = current_tick_object.center.y + delta.y;
            object->center_position.z = current_tick_object.center.z + delta.z;

            for(std::size_t n = 0; n < current_tick_object.node_count; n++) {
                auto &node = nodes[n];
                auto &node_current = current_tick_object.nodes[n];

                node.position.x = node_current.position.x + delta.x;
                node.position.y = node_current.position.y + delta.y;
                node.position.z = node_current.position.z + delta.z;

                // Do not interpolate the animation backwards in time.
                // Scale/rotation stay at the current authoritative tick.
                node.scale = node_current.scale;
                node.rotation = node_current.rotation;
            }

            return;
        }

        // Traditional previous-tick -> current-tick interpolation.
        interpolate_point(previous_tick_object.center, current_tick_object.center, object->center_position, interpolation_tick_progress);

        for(std::size_t n = 0; n < current_tick_object.node_count; n++) {
            auto &node = nodes[n];
            auto &node_current = current_tick_object.nodes[n];
            auto &node_before = previous_tick_object.nodes[n];

            interpolate_point(node_before.position, node_current.position, node.position, interpolation_tick_progress);

            node.scale = node_before.scale + (node_current.scale - node_before.scale) * interpolation_tick_progress;

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

            // Bipeds get a max speed of 2.5 per tick before they aren't interpolated. Other objects get 7.5 world units.
            static const float MAX_INTERPOLATION_DISTANCES[] = { 7.5*7.5, 2.5*2.5 };

            // Let's check if the distance between the two points is too great (such as if the object was teleported).
            current_tick_object.interpolate = distance_squared(current_tick_object.center, previous_tick[i].center) < MAX_INTERPOLATION_DISTANCES[object->type == OBJECT_TYPE_BIPED];

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
        }
    }

    void interpolate_object_on_tick() noexcept {
        tick_passed = true;
    }
}
