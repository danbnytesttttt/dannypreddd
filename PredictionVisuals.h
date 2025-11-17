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
    struct PredictionDrawData
    {
        math::vector3 source_position;      // Player position
        math::vector3 predicted_position;   // Where target will be
        math::vector3 cast_position;        // Where to aim spell
        float spell_radius;                 // Spell width/radius
        float timestamp;                    // When this prediction was made
        bool is_valid;                      // Whether this prediction is valid
        std::string target_name;            // Target champion name
        int spell_slot;                     // Which spell (Q=0, W=1, E=2, R=3)

        PredictionDrawData()
            : source_position(0, 0, 0)
            , predicted_position(0, 0, 0)
            , cast_position(0, 0, 0)
            , spell_radius(0.f)
            , timestamp(0.f)
            , is_valid(false)
            , target_name("")
            , spell_slot(-1)
        {}
    };

    // Settings
    struct VisualsSettings
    {
        bool enabled = true;
        bool draw_line = true;
        bool draw_circle = true;
        float max_age_seconds = 0.15f;  // Only draw predictions newer than this
        uint32_t line_color = 0xFF6060FF;     // Light red with alpha (ARGB format)
        uint32_t circle_color = 0xFF6060FF;   // Light red with alpha
        float line_thickness = 2.0f;
        float circle_thickness = 2.0f;

        static VisualsSettings& get()
        {
            static VisualsSettings instance;
            return instance;
        }
    };

    // Storage for most recent prediction per spell slot
    inline PredictionDrawData g_recent_predictions[4];  // Q, W, E, R

    /**
     * Store a prediction result for drawing
     */
    inline void store_prediction(
        const math::vector3& source_pos,
        const math::vector3& predicted_pos,
        const math::vector3& cast_pos,
        float spell_radius,
        int spell_slot,
        const std::string& target_name,
        float current_time)
    {
        if (!VisualsSettings::get().enabled)
            return;

        if (spell_slot < 0 || spell_slot > 3)
            return;

        auto& data = g_recent_predictions[spell_slot];
        data.source_position = source_pos;
        data.predicted_position = predicted_pos;
        data.cast_position = cast_pos;
        data.spell_radius = spell_radius;
        data.timestamp = current_time;
        data.is_valid = true;
        data.target_name = target_name;
        data.spell_slot = spell_slot;
    }

    /**
     * Draw all recent predictions
     */
    inline void draw_predictions(float current_time)
    {
        if (!VisualsSettings::get().enabled || !g_sdk || !g_sdk->renderer)
            return;

        const auto& settings = VisualsSettings::get();

        // Draw each spell slot's prediction if it's recent enough
        for (int i = 0; i < 4; ++i)
        {
            const auto& pred = g_recent_predictions[i];

            if (!pred.is_valid)
                continue;

            // Check if prediction is too old
            float age = current_time - pred.timestamp;
            if (age > settings.max_age_seconds)
                continue;

            // Draw line from source to predicted position
            if (settings.draw_line)
            {
                math::vector2 screen_start, screen_end;

                // Convert 3D positions to 2D screen coordinates
                if (g_sdk->renderer->world_to_screen(pred.source_position, screen_start) &&
                    g_sdk->renderer->world_to_screen(pred.cast_position, screen_end))
                {
                    g_sdk->renderer->add_line_2d(
                        screen_start,
                        screen_end,
                        settings.line_thickness,
                        settings.line_color
                    );
                }
            }

            // Draw circle at predicted cast position
            if (settings.draw_circle)
            {
                g_sdk->renderer->add_circle_3d(
                    pred.cast_position,
                    pred.spell_radius,
                    settings.circle_thickness,
                    settings.circle_color
                );
            }
        }
    }

    /**
     * Clear all stored predictions (call when game ends or plugin unloads)
     */
    inline void clear()
    {
        for (int i = 0; i < 4; ++i)
        {
            g_recent_predictions[i].is_valid = false;
        }
    }

} // namespace PredictionVisuals
