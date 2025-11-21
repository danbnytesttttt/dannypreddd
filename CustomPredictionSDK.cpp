#include "CustomPredictionSDK.h"
#include "EdgeCaseDetection.h"
#include "PredictionSettings.h"
#include "PredictionTelemetry.h"
#include "PredictionVisuals.h"
#include "FogOfWarTracker.h"
#include <algorithm>
#include <limits>
#include <sstream>
#include <chrono>

// Helper function to check if vector3 is zero/invalid (can't modify SDK math.hpp)
namespace math
{
    inline bool is_zero(const vector3& v)
    {
        constexpr float EPSILON = 1e-6f;
        return (std::fabs(v.x) < EPSILON && std::fabs(v.y) < EPSILON && std::fabs(v.z) < EPSILON);
    }
}

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
    // CRITICAL: Check target_selector is available before calling
    if (!sdk::target_selector)
    {
        result.hitchance = pred_sdk::hitchance::any;
        return result;
    }

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
    pred_sdk::pred_data result{};

    // Safety: Validate SDK is initialized
    if (!g_sdk || !g_sdk->object_manager)
    {
        result.hitchance = pred_sdk::hitchance::any;
        result.is_valid = false;
        return result;
    }

    if (PredictionSettings::get().enable_debug_logging)
    {
        char debug_msg[512];
        snprintf(debug_msg, sizeof(debug_msg),
            "[Danny.Prediction] Auto-target predict() - source=0x%p range=%.0f type=%d",
            (void*)spell_data.source, spell_data.range, static_cast<int>(spell_data.spell_type));
        g_sdk->log_console(debug_msg);
    }

    // If source is null, use local player as fallback
    if (!spell_data.source || !spell_data.source->is_valid())
    {
        spell_data.source = g_sdk->object_manager->get_local_player();
        if (PredictionSettings::get().enable_debug_logging)
            g_sdk->log_console("[Danny.Prediction] Auto-target: Using local player as source");
    }

    // Auto-select best target
    game_object* best_target = get_best_target(spell_data);

    if (!best_target)
    {
        if (PredictionSettings::get().enable_debug_logging)
            g_sdk->log_console("[Danny.Prediction] Auto-target: No valid target found");
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
    pred_sdk::pred_data result{};

    // Safety: Validate SDK is initialized
    if (!g_sdk || !g_sdk->object_manager || !g_sdk->clock_facade)
    {
        result.hitchance = pred_sdk::hitchance::any;
        result.is_valid = false;
        return result;
    }

    // FIXED: Use safe debug logging
    if (PredictionSettings::get().enable_debug_logging)
    {
        char debug_msg[256];
        snprintf(debug_msg, sizeof(debug_msg), "[Danny.Prediction] predict(target) called - obj=0x%p source=0x%p",
            (void*)obj, (void*)spell_data.source);
        g_sdk->log_console(debug_msg);
    }

    // Validation: obj must exist and be valid
    if (!obj || !obj->is_valid())
    {
        PRED_DEBUG_LOG("[Danny.Prediction] EARLY EXIT: Invalid target obj!");
        PredictionTelemetry::TelemetryLogger::log_rejection_invalid_target();
        result.hitchance = pred_sdk::hitchance::any;
        return result;
    }

    // If source is null/invalid, use local player as default (expected behavior for spell scripts)
    // This is normal - spell scripts typically don't set source, expecting prediction SDK to auto-fill
    if (!spell_data.source || !spell_data.source->is_valid())
    {
        spell_data.source = g_sdk->object_manager->get_local_player();

        // Validate local player is valid and has valid position
        if (!spell_data.source || !spell_data.source->is_valid())
        {
            // Should never happen - local player invalid
            g_sdk->log_console("[Danny.Prediction] CRITICAL ERROR: Local player is null or invalid!");
            result.hitchance = pred_sdk::hitchance::any;
            result.is_valid = false;
            return result;
        }

        // Validate position is not zero/invalid (sanity check)
        math::vector3 source_pos = spell_data.source->get_position();
        if (math::is_zero(source_pos))
        {
            // Local player has invalid position (zero vector)
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg),
                "[Danny.Prediction] ERROR: Local player position is zero! Source may be invalid.");
            g_sdk->log_console(err_msg);
            result.hitchance = pred_sdk::hitchance::any;
            result.is_valid = false;
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

    // CRITICAL: Check range BEFORE prediction to avoid wasting computation
    // Cache positions to prevent inconsistency from flash/dash
    math::vector3 source_pos = spell_data.source->get_position();
    math::vector3 target_pos = obj->get_position();
    float distance_to_target = target_pos.distance(source_pos);

    // FIX: Use target bounding radius dynamically (Cho'Gath = 100+, Malphite = 80, etc.)
    float target_radius = obj->get_bounding_radius();
    float effective_max_range = spell_data.range + target_radius + 25.f;  // 25 for buffer

    if (distance_to_target > effective_max_range)
    {
        if (PredictionSettings::get().enable_debug_logging)
        {
            char range_msg[256];
            snprintf(range_msg, sizeof(range_msg),
                "[Danny.Prediction] Target out of range: %.0f > %.0f (range + radius %.0f)",
                distance_to_target, effective_max_range, target_radius);
            g_sdk->log_console(range_msg);
        }
        PredictionTelemetry::TelemetryLogger::log_rejection_current_range();
        result.hitchance = pred_sdk::hitchance::any;
        result.is_valid = false;
        return result;
    }

    // CRITICAL: Check fog of war status
    float current_time = g_sdk->clock_facade->get_game_time();
    FogOfWarTracker::update_visibility(obj, current_time);

    auto [should_predict, fog_confidence_multiplier] = FogOfWarTracker::should_predict_target(obj, current_time);

    if (!should_predict)
    {
        // Target is in fog for too long - don't cast at stale position
        PredictionTelemetry::TelemetryLogger::log_rejection_fog();
        result.hitchance = pred_sdk::hitchance::any;
        result.is_valid = false;
        return result;
    }

    // Start telemetry timing
    auto telemetry_start = std::chrono::high_resolution_clock::now();

    // Use hybrid prediction system - wrapped in try-catch for safety
    HybridPred::HybridPredictionResult hybrid_result;
    try
    {
        hybrid_result = HybridPred::PredictionManager::predict(spell_data.source, obj, spell_data);
    }
    catch (...)
    {
        // Prediction system crashed - return invalid result
        result.hitchance = pred_sdk::hitchance::any;
        result.is_valid = false;
        return result;
    }

    // End telemetry timing
    auto telemetry_end = std::chrono::high_resolution_clock::now();
    float computation_time_ms = std::chrono::duration<float, std::milli>(telemetry_end - telemetry_start).count();

    // FIXED: Safe debug logging for prediction details
    if (PredictionSettings::get().enable_debug_logging)
    {
        try
        {
            std::stringstream ss;
            ss << "[Danny.Prediction] Target: " << obj->get_char_name()
                << " | Valid: " << (hybrid_result.is_valid ? "YES" : "NO")
                << " | HitChance: " << (hybrid_result.hit_chance * 100.f) << "% (" << hybrid_result.hit_chance << " raw)";
            g_sdk->log_console(ss.str().c_str());
        }
        catch (...) { /* Ignore logging errors */ }
    }

    if (!hybrid_result.is_valid)
    {
        if (!hybrid_result.reasoning.empty() && PredictionSettings::get().enable_debug_logging)
        {
            std::string debug_msg = "[Danny.Prediction] Reason invalid: " + hybrid_result.reasoning;
            g_sdk->log_console(debug_msg.c_str());
        }

        // Log invalid prediction to telemetry
        if (PredictionSettings::get().enable_telemetry)
        {
            PredictionTelemetry::TelemetryLogger::log_invalid_prediction(hybrid_result.reasoning);
        }

        result.hitchance = pred_sdk::hitchance::any;
        return result;
    }

    // Convert hybrid result to pred_data
    result = convert_to_pred_data(hybrid_result, obj, spell_data);

    // Apply fog of war confidence penalty
    if (fog_confidence_multiplier < 1.0f)
    {
        // Reduce hit chance for targets in fog
        float original_hc = hybrid_result.hit_chance;
        hybrid_result.hit_chance *= fog_confidence_multiplier;

        // Re-convert to enum with reduced hit chance
        result.hitchance = convert_hit_chance_to_enum(hybrid_result.hit_chance);

        if (PredictionSettings::get().enable_debug_logging)
        {
            char fog_msg[256];
            snprintf(fog_msg, sizeof(fog_msg),
                "[Danny.Prediction] FOG PENALTY: HC %.0f%% -> %.0f%% (multiplier: %.2f)",
                original_hc * 100.f, hybrid_result.hit_chance * 100.f, fog_confidence_multiplier);
            g_sdk->log_console(fog_msg);
        }
    }

    // DEFENSIVE PROGRAMMING: Enforce hitchance threshold at SDK level
    // This protects against buggy champion scripts that don't check hitchance properly
    bool should_cast = (result.hitchance >= spell_data.expected_hitchance);

    if (!should_cast)
    {
        if (PredictionSettings::get().enable_debug_logging)
        {
            char reject_msg[256];
            snprintf(reject_msg, sizeof(reject_msg),
                "[REJECT] Hitchance %d below threshold %d - invalidating prediction",
                result.hitchance, spell_data.expected_hitchance);
            g_sdk->log_console(reject_msg);
        }
        PredictionTelemetry::TelemetryLogger::log_rejection_hitchance();
        result.is_valid = false;
        result.hitchance = pred_sdk::hitchance::any;
        return result;
    }

    // CRITICAL: Validate predicted position is within spell range
    // This prevents casting at targets that will walk out of range
    float predicted_distance = result.cast_position.distance(source_pos);

    // For linear skillshots, add radius as buffer since we can hit along the path
    // For circular, be more strict since we need to reach the center
    float range_buffer = (spell_data.spell_type == pred_sdk::spell_type::linear) ? spell_data.radius : 25.f;
    float effective_range = spell_data.range + range_buffer;

    if (predicted_distance > effective_range)
    {
        if (PredictionSettings::get().enable_debug_logging)
        {
            char range_msg[256];
            snprintf(range_msg, sizeof(range_msg),
                "[REJECT] Predicted position out of range: %.0f > %.0f (range:%.0f + buffer:%.0f)",
                predicted_distance, effective_range, spell_data.range, range_buffer);
            g_sdk->log_console(range_msg);
        }
        PredictionTelemetry::TelemetryLogger::log_rejection_predicted_range();
        result.is_valid = false;
        result.hitchance = pred_sdk::hitchance::any;
        return result;
    }

    // Check collision if required
    if (!spell_data.forbidden_collisions.empty())
    {
        try
        {
            pred_sdk::collision_ret collision = collides(result.cast_position, spell_data, obj);
            if (collision.collided)
            {
                // CRITICAL: For non-piercing skillshots, ANY collision invalidates the prediction
                PredictionTelemetry::TelemetryLogger::log_rejection_collision();
                result.is_valid = false;
                result.hitchance = pred_sdk::hitchance::any;
                return result;
            }
        }
        catch (...) { /* Ignore collision check errors */ }
    }

    // Log successful prediction to telemetry (wrapped in try-catch for safety)
    if (PredictionSettings::get().enable_telemetry)
    {
        try
        {
        PredictionTelemetry::PredictionEvent event;
        event.timestamp = g_sdk->clock_facade->get_game_time();
        event.target_name = obj->get_char_name();

        // Map spell type enum to string
        switch (spell_data.spell_type)
        {
        case pred_sdk::spell_type::linear: event.spell_type = "linear"; break;
        case pred_sdk::spell_type::circular: event.spell_type = "circular"; break;
        case pred_sdk::spell_type::targetted: event.spell_type = "targeted"; break;
        case pred_sdk::spell_type::vector: event.spell_type = "vector"; break;
        default: event.spell_type = "unknown"; break;
        }

        event.hit_chance = hybrid_result.hit_chance;
        event.confidence = hybrid_result.confidence_score;
        event.distance = spell_data.source->get_position().distance(obj->get_position());
        event.computation_time_ms = computation_time_ms;

        // Spell configuration data (for diagnosing misconfigured spells)
        event.spell_range = spell_data.range;
        event.spell_radius = spell_data.radius;
        event.spell_delay = spell_data.delay;
        event.spell_speed = spell_data.projectile_speed;

        // Movement and prediction offset data
        math::vector3 current_pos = obj->get_position();
        math::vector3 predicted_pos = result.cast_position;
        event.prediction_offset = predicted_pos.distance(current_pos);
        event.target_velocity = obj->get_move_speed();

        // Check if target is moving by examining path
        auto path = obj->get_path();
        event.target_is_moving = (path.size() > 1);

        // Extract edge case info from reasoning
        if (hybrid_result.reasoning.find("STASIS") != std::string::npos)
            event.edge_case = "stasis";
        else if (hybrid_result.reasoning.find("CHANNEL") != std::string::npos ||
            hybrid_result.reasoning.find("RECALL") != std::string::npos)
            event.edge_case = "channeling";
        else if (hybrid_result.reasoning.find("DASH") != std::string::npos)
        {
            event.edge_case = "dash";
            event.was_dash = true;
        }
        else
            event.edge_case = "normal";

        // Check for stationary/animation lock
        event.was_stationary = hybrid_result.reasoning.find("STATIONARY") != std::string::npos;
        event.was_animation_locked = hybrid_result.reasoning.find("animation") != std::string::npos ||
            hybrid_result.reasoning.find("LOCKED") != std::string::npos;
        event.collision_detected = false;  // Will be updated if collision check fails

        PredictionTelemetry::TelemetryLogger::log_prediction(event);
        }
        catch (...) { /* Ignore telemetry errors */ }
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
        if (path.size() > 1)
        {
            // FIX: Use NEXT immediate waypoint, not final destination
            // Prevents "corner cutting" through walls on L-shaped paths
            // path[0] = current pos, path[1] = next corner/waypoint
            math::vector3 waypoint = path[1];
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

    // FIX: Allow small buffer for edge-of-range hits
    // For linear spells, the hitbox extends beyond the center by spell radius
    // Example: 1000 range spell with 60 radius can hit at 1060 (edge hit)
    constexpr float EDGE_HIT_BUFFER = 50.f;
    return distance <= effective_range + EDGE_HIT_BUFFER;
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
    // CRITICAL: Validate SDK before any operations
    if (!g_sdk || !g_sdk->object_manager)
        return nullptr;

    if (!spell_data.source || !spell_data.source->is_valid())
    {
        if (PredictionSettings::get().enable_debug_logging && g_sdk)
            g_sdk->log_console("[Danny.Prediction] get_best_target: Invalid source");
        return nullptr;
    }

    // Calculate search range: Tactical vs Global spells
    // TACTICAL (< 2500): Add buffer for enemies walking into range
    // GLOBAL (>= 2500): Use full range, let hitchance filter bad shots
    float search_range = spell_data.range;
    if (spell_data.range < 2500.f)
    {
        // Add buffer scaled to spell range, capped at 300 units
        // Example: 1000 range spell gets +300 buffer = 1300 search range
        //          500 range spell gets +250 buffer = 750 search range
        float buffer = std::min(spell_data.range * 0.5f, 300.f);
        search_range = spell_data.range + buffer;
    }
    // else: Global spell - use full range (Jinx R 25000, Ashe R 20000, etc.)

    // Use SDK target selector with range filter - let it handle priority/threat logic
    if (sdk::target_selector)
    {
        // Pass filter to target selector - TS handles priority, we constrain range
        auto* ts_target = sdk::target_selector->get_hero_target([&](game_object* obj) -> bool {
            if (!obj || !obj->is_valid() || obj->is_dead())
                return false;

            float distance = obj->get_position().distance(spell_data.source->get_position());
            return distance <= search_range;
        });

        if (ts_target)
        {
            float distance = ts_target->get_position().distance(spell_data.source->get_position());

            // If target is currently in ability range, accept immediately
            if (distance <= spell_data.range)
            {
                if (PredictionSettings::get().enable_debug_logging)
                {
                    char debug[256];
                    sprintf_s(debug, "[Danny.Prediction] Using TS target at %.0f units (in range)", distance);
                    g_sdk->log_console(debug);
                }
                return ts_target;
            }

            // Target is in buffer zone (between range and search_range)
            // Only accept if they're moving toward us (buffer is for incoming enemies)
            if (spell_data.range < 2500.f)  // Only check for tactical spells with buffer
            {
                auto path = ts_target->get_path();
                if (path.size() > 1)
                {
                    // Check if target is moving toward us
                    math::vector3 current_pos = ts_target->get_position();
                    math::vector3 next_waypoint = path[1];
                    math::vector3 source_pos = spell_data.source->get_position();

                    float current_distance = current_pos.distance(source_pos);
                    float next_distance = next_waypoint.distance(source_pos);

                    if (next_distance < current_distance)
                    {
                        // Moving toward us - accept
                        if (PredictionSettings::get().enable_debug_logging)
                        {
                            char debug[256];
                            sprintf_s(debug, "[Danny.Prediction] Using TS target at %.0f units (moving into range)", distance);
                            g_sdk->log_console(debug);
                        }
                        return ts_target;
                    }
                }

                // Target is in buffer but not moving toward us - reject, search for closer target
                if (PredictionSettings::get().enable_debug_logging)
                {
                    char debug[256];
                    sprintf_s(debug, "[Danny.Prediction] TS target at %.0f units not moving into range, searching alternatives", distance);
                    g_sdk->log_console(debug);
                }
            }
            else
            {
                // Global spell - always accept (they're in search range)
                return ts_target;
            }
        }
    }

    // Fallback: Find best target based on hybrid prediction score (prioritizes CLOSE targets)
    game_object* best_target = nullptr;
    float best_score = -1.f;

    auto all_heroes = g_sdk->object_manager->get_heroes();

    for (auto* hero : all_heroes)
    {
        if (!hero || !hero->is_valid() || hero->is_dead() || hero->get_team_id() == spell_data.source->get_team_id())
            continue;

        // Check range (same as TS filter for consistency)
        float distance = hero->get_position().distance(spell_data.source->get_position());
        if (distance > search_range)
            continue;

        // Calculate score
        float score = calculate_target_score(hero, spell_data);

        // BONUS: Small preference for targets currently in range (tiebreaker, not override)
        if (distance <= spell_data.range)
        {
            score *= 1.15f;  // 15% bonus - allows high hitchance buffer targets to still win
        }

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

    // FIXED: HEAVILY prioritize closer targets to avoid cross-map targeting
    float distance = target->get_position().distance(spell_data.source->get_position());
    float distance_factor = 0.f;
    if (spell_data.range > 0.f)
    {
        distance_factor = 1.f - std::min(distance / spell_data.range, 1.f);
    }

    // Increased proximity weight from 30% to 70% - nearby targets get MUCH higher score
    // Old: 0.7 + 0.3*factor (70%-100%)
    // New: 0.3 + 0.7*factor (30%-100%)
    // Example: Target at max range gets 0.3x multiplier (70% penalty)
    //          Target at half range gets 0.65x multiplier (35% penalty)
    //          Target at point blank gets 1.0x multiplier (no penalty)
    score *= (0.3f + distance_factor * 0.7f);

    return score;
}

bool CustomPredictionSDK::check_collision_simple(
    const math::vector3& start,
    const math::vector3& end,
    const pred_sdk::spell_data& spell_data,
    const game_object* target_obj)
{
    // CRITICAL: Validate SDK before checking collisions
    if (!g_sdk || !g_sdk->object_manager)
        return false;  // No collision if can't check

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

                // Only ENEMY minions block skillshots
                if (minion->get_team_id() == spell_data.source->get_team_id())
                    continue;

                // Skip wards
                std::string name = minion->get_char_name();
                if (name.find("Ward") != std::string::npos ||
                    name.find("Trinket") != std::string::npos ||
                    name.find("YellowTrinket") != std::string::npos)
                    continue;

                // Point-to-line distance check
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
                {
                    return true;  // Collision detected
                }
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

                // Only ENEMY heroes block skillshots
                if (hero->get_team_id() == spell_data.source->get_team_id())
                    continue;

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
                {
                    return true;  // Collision detected
                }
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
