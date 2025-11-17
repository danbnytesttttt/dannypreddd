#pragma once

#include "sdk.hpp"
#include <chrono>

/**
 * =============================================================================
 * PREDICTION VISUALIZATION SYSTEM
 * =============================================================================
 *
 * Draws real-time prediction indicators:
 * - Line from player to predicted position
 * - Circle at predicted cast position with spell radius
 *
 * =============================================================================
 */

namespace PredictionVisuals
{
    // Settings
    struct VisualsSettings
    {
        bool enabled = true;
        bool draw_current_position = true;       // Draw circle at enemy's current position
        bool draw_predicted_position = true;     // Draw circle at predicted position
        bool draw_movement_line = true;          // Draw line current → predicted
        float prediction_time = 0.75f;           // How far ahead to predict (seconds)

        uint32_t current_pos_color = 0xFF00FF00;    // Green - where enemy is NOW
        uint32_t predicted_pos_color = 0xFFFF6060;  // Red - where they'll be
        uint32_t movement_line_color = 0xFFFFFF00;  // Yellow - movement direction

        float current_circle_radius = 65.0f;
        float predicted_circle_radius = 80.0f;
        float line_thickness = 2.0f;
        float circle_thickness = 2.5f;

        static VisualsSettings& get()
        {
            static VisualsSettings instance;
            return instance;
        }
    };

    /**
     * Update and draw continuous prediction for current target
     */
    inline void draw_continuous_prediction(float current_time)
    {
        // Safety checks
        if (!VisualsSettings::get().enabled)
            return;

        if (!g_sdk || !g_sdk->renderer)
            return;

        if (!sdk::target_selector)
            return;

        // Get current target from target selector with additional safety
        auto* target = sdk::target_selector->get_hero_target();
        if (!target)
            return;

        // CRITICAL: Additional validity checks to prevent crashes
        if (!target->is_valid())
            return;

        if (target->is_dead())
            return;

        if (!target->is_visible())
            return;

        const auto& settings = VisualsSettings::get();

        // Get current position and velocity with exception handling
        math::vector3 current_pos;
        math::vector3 velocity;

        try
        {
            current_pos = target->get_position();
            velocity = target->get_velocity();
        }
        catch (...)
        {
            // Target became invalid mid-frame
            return;
        }

        // Simple linear prediction: position + velocity * time
        math::vector3 predicted_pos = current_pos + velocity * settings.prediction_time;

        // Draw current position (green circle)
        if (settings.draw_current_position)
        {
            try
            {
                g_sdk->renderer->add_circle_3d(
                    current_pos,
                    settings.current_circle_radius,
                    settings.circle_thickness,
                    settings.current_pos_color
                );
            }
            catch (...) { /* Ignore render errors */ }
        }

        // Draw predicted position (red circle)
        if (settings.draw_predicted_position)
        {
            try
            {
                g_sdk->renderer->add_circle_3d(
                    predicted_pos,
                    settings.predicted_circle_radius,
                    settings.circle_thickness,
                    settings.predicted_pos_color
                );
            }
            catch (...) { /* Ignore render errors */ }
        }

        // Draw movement line (yellow line from current to predicted)
        if (settings.draw_movement_line)
        {
            try
            {
                math::vector2 screen_current = g_sdk->renderer->world_to_screen(current_pos);
                math::vector2 screen_predicted = g_sdk->renderer->world_to_screen(predicted_pos);

                bool current_valid = (screen_current.x != 0.f || screen_current.y != 0.f);
                bool predicted_valid = (screen_predicted.x != 0.f || screen_predicted.y != 0.f);

                if (current_valid && predicted_valid)
                {
                    g_sdk->renderer->add_line_2d(
                        screen_current,
                        screen_predicted,
                        settings.line_thickness,
                        settings.movement_line_color
                    );
                }
            }
            catch (...) { /* Ignore render errors */ }
        }
    }

    /**
     * Clear all resources (call when game ends or plugin unloads)
     */
    inline void clear()
    {
        // Nothing to clear for continuous prediction system
    }

} // namespace PredictionVisuals
