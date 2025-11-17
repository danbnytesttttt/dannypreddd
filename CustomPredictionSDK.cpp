#include "CustomPredictionSDK.h"
#include "EdgeCaseDetection.h"
#include "PredictionSettings.h"
#include <algorithm>
#include <limits>
#include <sstream>

// =============================================================================
// TARGETED SPELL PREDICTION
// =============================================================================

pred_sdk::pred_data CustomPredictionSDK::targetted(pred_sdk::spell_data spell_data)
{
    PRED_DEBUG_LOG("[Danny.Prediction] targetted() called (point-and-click spell)");

    pred_sdk::pred_data result{};

    // Targeted spells don't need prediction - just return target position
    if (!spell_data.source || !spell_data.source->is_valid())
    {
        result.hitchance = pred_sdk::hitchance::any;
        return result;
    }

    // Use target selector to find best target
    auto* target = sdk::target_selector->get_hero_target();

    if (!target || !target->is_valid())
    {
        result.hitchance = pred_sdk::hitchance::any;
        return result;
    }

    // For targeted spells, prediction is trivial
    result.cast_position = target->get_position();
    result.predicted_position = target->get_position();
    result.hitchance = pred_sdk::hitchance::very_high;
    result.target = target;
    result.is_valid = true;

    return result;
}

// =============================================================================
// SKILLSHOT PREDICTION (AUTO-TARGET)
// =============================================================================

pred_sdk::pred_data CustomPredictionSDK::predict(pred_sdk::spell_data spell_data)
{
    PRED_DEBUG_LOG("[Danny.Prediction] Auto-target predict() called");

    // Auto-select best target
    game_object* best_target = get_best_target(spell_data);

    if (!best_target)
    {
        PRED_DEBUG_LOG("[Danny.Prediction] Auto-target: No valid target found");
        pred_sdk::pred_data result{};
        result.hitchance = pred_sdk::hitchance::any;
        return result;
    }

    // FIXED: Use safe string formatting to prevent buffer overflow
    if (PredictionSettings::get().enable_debug_logging)
    {
        std::string debug_msg = "[Danny.Prediction] Auto-target selected: " + best_target->get_char_name();
        g_sdk->log_console(debug_msg.c_str());
    }

    return predict(best_target, spell_data);
}

// =============================================================================
// SKILLSHOT PREDICTION (SPECIFIC TARGET)
// =============================================================================

pred_sdk::pred_data CustomPredictionSDK::predict(game_object* obj, pred_sdk::spell_data spell_data)
{
    // FIXED: Use safe debug logging
    if (PredictionSettings::get().enable_debug_logging)
    {
        char debug_msg[256];
        snprintf(debug_msg, sizeof(debug_msg), "[Danny.Prediction] predict(target) called - obj=0x%p source=0x%p",
            (void*)obj, (void*)spell_data.source);
        g_sdk->log_console(debug_msg);
    }

    pred_sdk::pred_data result{};

    // Validation: obj must exist and be valid
    if (!obj || !obj->is_valid())
    {
        PRED_DEBUG_LOG("[Danny.Prediction] EARLY EXIT: Invalid target obj!");
        result.hitchance = pred_sdk::hitchance::any;
        return result;
    }

    // CRITICAL FIX: If source is null, use local player as default
    if (!spell_data.source || !spell_data.source->is_valid())
    {
        static bool warned_once = false;
        if (!warned_once)
        {
            g_sdk->log_console("[Danny.Prediction] WARNING: source was null - using local player (will not warn again)");
            warned_once = true;
        }

        spell_data.source = g_sdk->object_manager->get_local_player();

        if (!spell_data.source || !spell_data.source->is_valid())
        {
            g_sdk->log_console("[Danny.Prediction] EARLY EXIT: Could not get valid source!");
            result.hitchance = pred_sdk::hitchance::any;
            return result;
        }
    }

    // FIXED: Safe debug logging for spell details
    if (PredictionSettings::get().enable_debug_logging)
    {
        char debug_msg[256];
        snprintf(debug_msg, sizeof(debug_msg), "[Danny.Prediction] Spell: Range=%.0f Radius=%.0f Delay=%.2f Speed=%.0f Type=%d",
            spell_data.range, spell_data.radius, spell_data.delay, spell_data.projectile_speed,
            static_cast<int>(spell_data.spell_type));
        g_sdk->log_console(debug_msg);
    }

    // Use hybrid prediction system
    HybridPred::HybridPredictionResult hybrid_result =
        HybridPred::PredictionManager::predict(spell_data.source, obj, spell_data);

    // FIXED: Safe debug logging for prediction details
    if (PredictionSettings::get().enable_debug_logging)
    {
        std::stringstream ss;
        ss << "[Danny.Prediction] Target: " << obj->get_char_name()
           << " | Valid: " << (hybrid_result.is_valid ? "YES" : "NO")
           << " | HitChance: " << (hybrid_result.hit_chance * 100.f) << "% (" << hybrid_result.hit_chance << " raw)";
        g_sdk->log_console(ss.str().c_str());
    }

    if (!hybrid_result.is_valid)
    {
        if (!hybrid_result.reasoning.empty() && PredictionSettings::get().enable_debug_logging)
        {
            std::string debug_msg = "[Danny.Prediction] Reason invalid: " + hybrid_result.reasoning;
            g_sdk->log_console(debug_msg.c_str());
        }
        result.hitchance = pred_sdk::hitchance::any;
        return result;
    }

    // Convert hybrid result to pred_data
    result = convert_to_pred_data(hybrid_result, obj, spell_data);

    // DEBUG: Log final hitchance with detailed comparison
    const char* hc_name = "UNKNOWN";
    switch (result.hitchance) {
        case pred_sdk::hitchance::guaranteed_hit: hc_name = "GUARANTEED_HIT(100)"; break;
        case pred_sdk::hitchance::very_high: hc_name = "VERY_HIGH(85)"; break;
        case pred_sdk::hitchance::high: hc_name = "HIGH(70)"; break;
        case pred_sdk::hitchance::medium: hc_name = "MEDIUM(50)"; break;
        case pred_sdk::hitchance::low: hc_name = "LOW(30)"; break;
        case pred_sdk::hitchance::any: hc_name = "ANY(0)"; break;
    }

    const char* thresh_name = "UNKNOWN";
    switch (spell_data.expected_hitchance) {
        case pred_sdk::hitchance::guaranteed_hit: thresh_name = "GUARANTEED_HIT(100)"; break;
        case pred_sdk::hitchance::very_high: thresh_name = "VERY_HIGH(85)"; break;
        case pred_sdk::hitchance::high: thresh_name = "HIGH(70)"; break;
        case pred_sdk::hitchance::medium: thresh_name = "MEDIUM(50)"; break;
        case pred_sdk::hitchance::low: thresh_name = "LOW(30)"; break;
        case pred_sdk::hitchance::any: thresh_name = "ANY(0)"; break;
    }

    bool should_cast = (result.hitchance >= spell_data.expected_hitchance);

    // FIXED: Safe debug logging
    if (PredictionSettings::get().enable_debug_logging)
    {
        std::stringstream ss;
        ss << "[Danny.Prediction] Hitchance: " << hc_name << " | Threshold: " << thresh_name
           << " | SHOULD_CAST: " << (should_cast ? "YES!!!" : "NO (too low)");
        g_sdk->log_console(ss.str().c_str());

        if (should_cast) {
            char pos_msg[128];
            snprintf(pos_msg, sizeof(pos_msg), "[Danny.Prediction] >>> CAST POSITION: (%.1f, %.1f, %.1f) <<<",
                result.cast_position.x, result.cast_position.y, result.cast_position.z);
            g_sdk->log_console(pos_msg);
        } else {
            // Suggest if threshold might be too conservative
            if (spell_data.expected_hitchance >= pred_sdk::hitchance::very_high) {
                g_sdk->log_console("[Danny.Prediction] HINT: Threshold is VERY_HIGH or GUARANTEED - try lowering to HIGH or MEDIUM in script settings");
            }
        }
    }

    // Check collision if required
    if (!spell_data.forbidden_collisions.empty())
    {
        pred_sdk::collision_ret collision = collides(result.cast_position, spell_data, obj);
        if (collision.collided)
        {
            // CRITICAL: For non-piercing skillshots, ANY collision invalidates the prediction
            // Don't just reduce hitchance - completely block the cast
            PRED_DEBUG_LOG("[Danny.Prediction] COLLISION DETECTED - Minion blocking path!");
            PRED_DEBUG_LOG("[Danny.Prediction] Invalidating prediction - cannot hit through minions");
            result.is_valid = false;
            result.hitchance = pred_sdk::hitchance::any;
            return result;
        }
        else
        {
            PRED_DEBUG_LOG("[Danny.Prediction] No collision detected - path is clear");
        }
    }

    return result;
}

// =============================================================================
// PATH PREDICTION (LINEAR)
// =============================================================================

math::vector3 CustomPredictionSDK::predict_on_path(game_object* obj, float time, bool use_server_pos)
{
    if (!obj || !obj->is_valid())
        return math::vector3{};

    // Get current position
    math::vector3 position = use_server_pos ? obj->get_server_position() : obj->get_position();

    // Get tracker for velocity data
    auto* tracker = HybridPred::PredictionManager::get_tracker(obj);
    if (!tracker)
    {
        // Fallback to simple prediction using path
        auto path = obj->get_path();
        if (!path.empty())
        {
            math::vector3 waypoint = path[path.size() - 1];
            math::vector3 direction = (waypoint - position).normalized();
            return position + direction * (obj->get_move_speed() * time);
        }

        // No path, just return current position
        return position;
    }

    // Use physics predictor with current velocity
    math::vector3 current_velocity = tracker->get_current_velocity();
    return HybridPred::PhysicsPredictor::predict_linear_position(
        position,
        current_velocity,
        time
    );
}

// =============================================================================
// COLLISION DETECTION
// =============================================================================

pred_sdk::collision_ret CustomPredictionSDK::collides(
    const math::vector3& end_point,
    pred_sdk::spell_data spell_data,
    const game_object* target_obj)
{
    pred_sdk::collision_ret result{};
    result.collided = false;

    if (spell_data.forbidden_collisions.empty())
        return result;

    if (!spell_data.source || !spell_data.source->is_valid())
        return result;

    math::vector3 start = spell_data.source->get_position();

    // Simple collision check
    if (check_collision_simple(start, end_point, spell_data, target_obj))
    {
        result.collided = true;
        result.collided_units.clear(); // Simplified - would need actual collision units
    }

    return result;
}

// =============================================================================
// UTILITY FUNCTIONS IMPLEMENTATION
// =============================================================================

float CustomPredictionSDK::CustomPredictionUtils::get_spell_range(
    pred_sdk::spell_data& data,
    game_object* target,
    game_object* source)
{
    if (!source)
        source = data.source;

    if (!source || !source->is_valid())
        return data.range;

    float base_range = data.range;

    // Adjust for targeting type
    if (target && target->is_valid())
    {
        if (data.targetting_type == pred_sdk::targetting_type::center_to_edge)
        {
            // Add target bounding radius
            base_range += target->get_bounding_radius();
        }
        else if (data.targetting_type == pred_sdk::targetting_type::edge_to_edge)
        {
            // Add both source and target bounding radius
            base_range += source->get_bounding_radius();
            base_range += target->get_bounding_radius();
        }
    }

    return base_range;
}

bool CustomPredictionSDK::CustomPredictionUtils::is_in_range(
    pred_sdk::spell_data& data,
    math::vector3 cast_position,
    game_object* target)
{
    if (!data.source || !data.source->is_valid())
        return false;

    math::vector3 source_pos = data.source->get_position();
    float distance = source_pos.distance(cast_position);

    float effective_range = get_spell_range(data, target, data.source);

    return distance <= effective_range;
}

float CustomPredictionSDK::CustomPredictionUtils::get_spell_hit_time(
    pred_sdk::spell_data& data,
    math::vector3 pos,
    game_object* target)
{
    if (!data.source || !data.source->is_valid())
        return 0.f;

    return HybridPred::PhysicsPredictor::compute_arrival_time(
        data.source->get_position(),
        pos,
        data.projectile_speed,
        data.delay
    );
}

float CustomPredictionSDK::CustomPredictionUtils::get_spell_escape_time(
    pred_sdk::spell_data& data,
    game_object* target)
{
    if (!target || !target->is_valid() || !data.source || !data.source->is_valid())
        return 0.f;

    float current_distance = target->get_position().distance(data.source->get_position());
    float spell_range = get_spell_range(data, target, data.source);

    if (current_distance >= spell_range)
        return 0.f; // Already out of range

    float distance_to_escape = spell_range - current_distance;
    float move_speed = target->get_move_speed();

    // FIXED: Guard against zero/very low move speed (CC'd, dead, etc.)
    if (move_speed < 1.f)
        return std::numeric_limits<float>::max(); // Effectively can't escape

    return distance_to_escape / move_speed;
}

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

pred_sdk::pred_data CustomPredictionSDK::convert_to_pred_data(
    const HybridPred::HybridPredictionResult& hybrid_result,
    game_object* target,
    const pred_sdk::spell_data& spell_data)
{
    pred_sdk::pred_data result{};

    // Copy positions
    result.cast_position = hybrid_result.cast_position;
    result.first_cast_position = hybrid_result.first_cast_position;  // For vector spells (Viktor E, Rumble R, Irelia E)
    result.predicted_position = hybrid_result.cast_position;
    result.target = target;
    result.is_valid = hybrid_result.is_valid;

    // Convert hit chance to enum
    result.hitchance = convert_hit_chance_to_enum(hybrid_result.hit_chance);

    // Calculate hit time
    if (spell_data.source && spell_data.source->is_valid())
    {
        result.intersection_time = HybridPred::PhysicsPredictor::compute_arrival_time(
            spell_data.source->get_position(),
            hybrid_result.cast_position,
            spell_data.projectile_speed,
            spell_data.delay
        );
    }

    return result;
}

pred_sdk::hitchance CustomPredictionSDK::convert_hit_chance_to_enum(float hit_chance)
{
    // Map [0,1] to hitchance enum
    // Adjusted thresholds for more aggressive casting (less "holding")
    if (hit_chance >= 0.95f)
        return pred_sdk::hitchance::guaranteed_hit;
    else if (hit_chance >= 0.80f)  // very_high: kept at 80%
        return pred_sdk::hitchance::very_high;
    else if (hit_chance >= 0.65f)  // high: kept at 65%
        return pred_sdk::hitchance::high;
    else if (hit_chance >= 0.50f)  // medium: 50%
        return pred_sdk::hitchance::medium;
    else if (hit_chance >= 0.30f)  // low: 30%
        return pred_sdk::hitchance::low;
    else
        return pred_sdk::hitchance::any;
}

game_object* CustomPredictionSDK::get_best_target(const pred_sdk::spell_data& spell_data)
{
    if (!spell_data.source || !spell_data.source->is_valid())
        return nullptr;

    // Use target selector if available
    if (sdk::target_selector)
    {
        return sdk::target_selector->get_hero_target();
    }

    // Fallback: Find best target based on hybrid prediction score
    game_object* best_target = nullptr;
    float best_score = -1.f;

    auto all_heroes = g_sdk->object_manager->get_heroes();
    for (auto* hero : all_heroes)
    {
        if (!hero || !hero->is_valid() || hero->is_dead() || hero->get_team_id() == spell_data.source->get_team_id())
            continue;

        // Check range
        float distance = hero->get_position().distance(spell_data.source->get_position());
        if (distance > spell_data.range + 200.f) // Add buffer
            continue;

        // Calculate score
        float score = calculate_target_score(hero, spell_data);

        if (score > best_score)
        {
            best_score = score;
            best_target = hero;
        }
    }

    return best_target;
}

float CustomPredictionSDK::calculate_target_score(
    game_object* target,
    const pred_sdk::spell_data& spell_data)
{
    if (!target || !target->is_valid())
        return 0.f;

    // Analyze edge cases for this target
    EdgeCases::EdgeCaseAnalysis edge_cases = EdgeCases::analyze_target(target, spell_data.source);

    // Filter out invalid targets
    if (edge_cases.is_clone)
        return 0.f;  // Don't target clones

    if (edge_cases.blocked_by_windwall)
        return 0.f;  // Can't hit through windwall

    // Get hybrid prediction for this target
    HybridPred::HybridPredictionResult pred_result =
        HybridPred::PredictionManager::predict(spell_data.source, target, spell_data);

    if (!pred_result.is_valid)
        return 0.f;

    float score = pred_result.hit_chance;

    // Apply edge case priority multipliers (HUGE impact)
    score *= edge_cases.priority_multiplier;

    // Prioritize closer targets
    float distance = target->get_position().distance(spell_data.source->get_position());
    float distance_factor = 1.f - std::min(distance / spell_data.range, 1.f);
    score *= (0.7f + distance_factor * 0.3f);

    return score;
}

bool CustomPredictionSDK::check_collision_simple(
    const math::vector3& start,
    const math::vector3& end,
    const pred_sdk::spell_data& spell_data,
    const game_object* target_obj)
{
    // Check each collision type
    for (auto collision_type : spell_data.forbidden_collisions)
    {
        if (collision_type == pred_sdk::collision_type::unit)
        {
            auto minions = g_sdk->object_manager->get_minions();
            for (auto* minion : minions)
            {
                if (!minion || minion == target_obj)
                    continue;

                if (!is_collision_object(minion, spell_data))
                    continue;

                // FIXED: Check collision with BOTH ally and enemy minions
                // In League of Legends, both ally and enemy minions block non-piercing skillshots
                // (Blitz Q, Thresh Q, Lux Q, Morgana Q, Ezreal Q, etc.)
                // Piercing behavior should be handled by NOT adding collision_type::unit to forbidden_collisions

                // Simple point-to-line distance check
                math::vector3 minion_pos = minion->get_position();
                math::vector3 line_dir = (end - start).normalized();
                math::vector3 to_minion = minion_pos - start;

                float projection = to_minion.dot(line_dir);
                float line_length = start.distance(end);

                if (projection < 0.f || projection > line_length)
                    continue;

                math::vector3 closest_point = start + line_dir * projection;
                float distance = minion_pos.distance(closest_point);

                if (distance <= spell_data.radius + minion->get_bounding_radius())
                    return true;
            }
        }
        else if (collision_type == pred_sdk::collision_type::hero)
        {
            auto heroes = g_sdk->object_manager->get_heroes();
            for (auto* hero : heroes)
            {
                if (!hero || hero == target_obj || hero == spell_data.source)
                    continue;

                if (!is_collision_object(hero, spell_data))
                    continue;

                // FIXED: Check collision with BOTH ally and enemy heroes
                // Both block non-piercing skillshots in League of Legends

                math::vector3 hero_pos = hero->get_position();
                math::vector3 line_dir = (end - start).normalized();
                math::vector3 to_hero = hero_pos - start;

                float projection = to_hero.dot(line_dir);
                float line_length = start.distance(end);

                if (projection < 0.f || projection > line_length)
                    continue;

                math::vector3 closest_point = start + line_dir * projection;
                float distance = hero_pos.distance(closest_point);

                if (distance <= spell_data.radius + hero->get_bounding_radius())
                    return true;
            }
        }
        // Terrain collision skipped - would need navmesh API
    }

    return false;
}

bool CustomPredictionSDK::is_collision_object(
    game_object* obj,
    const pred_sdk::spell_data& spell_data)
{
    if (!obj || !obj->is_valid() || obj->is_dead())
        return false;

    // Object must be targetable
    if (!obj->is_targetable())
        return false;

    // Check visibility
    if (!obj->is_visible())
        return false;

    return true;
}