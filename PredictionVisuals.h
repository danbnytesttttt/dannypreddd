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
        float max_age_seconds = 2.0f;  // Only draw predictions newer than this (increased from 0.15s)
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
        {
            if (g_sdk)
            {
                static bool logged_disabled = false;
                if (!logged_disabled)
                {
                    g_sdk->log_console("[PredVisuals] Visuals disabled in settings");
                    logged_disabled = true;
                }
            }
            return;
        }

        // Debug: Log what spell_slot we received
        if (g_sdk)
        {
            static int debug_count = 0;
            if (debug_count++ < 3)  // Log first 3 calls
            {
                char msg[256];
                snprintf(msg, sizeof(msg), "[PredVisuals] store_prediction() called - spell_slot=%d, radius=%.0f",
                    spell_slot, spell_radius);
                g_sdk->log_console(msg);
            }
        }

        // If spell_slot is invalid, use slot 0 as fallback so visuals still work
        if (spell_slot < 0 || spell_slot > 3)
        {
            if (g_sdk)
            {
                static bool logged_fallback = false;
                if (!logged_fallback)
                {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "[PredVisuals] Invalid spell_slot %d, using slot 0 as fallback", spell_slot);
                    g_sdk->log_console(msg);
                    logged_fallback = true;
                }
            }
            spell_slot = 0;  // Fallback to Q slot
        }

        auto& data = g_recent_predictions[spell_slot];
        data.source_position = source_pos;
        data.predicted_position = predicted_pos;
        data.cast_position = cast_pos;
        data.spell_radius = spell_radius;
        data.timestamp = current_time;
        data.is_valid = true;
        data.target_name = target_name;
        data.spell_slot = spell_slot;

        // Debug logging (once per spell slot change)
        static int last_logged_slot = -1;
        if (g_sdk && spell_slot != last_logged_slot)
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "[PredVisuals] Stored prediction for slot %d (radius=%.0f)", spell_slot, spell_radius);
            g_sdk->log_console(msg);
            last_logged_slot = spell_slot;
        }
    }

    /**
     * Draw all recent predictions
     */
    inline void draw_predictions(float current_time)
    {
        if (!VisualsSettings::get().enabled || !g_sdk || !g_sdk->renderer)
            return;

        const auto& settings = VisualsSettings::get();

        // Debug: Log draw call (once every 5 seconds)
        static float last_draw_log = 0.f;
        if (g_sdk && current_time - last_draw_log > 5.0f)
        {
            int valid_count = 0;
            for (int i = 0; i < 4; ++i)
                if (g_recent_predictions[i].is_valid) valid_count++;

            char msg[256];
            snprintf(msg, sizeof(msg), "[PredVisuals] draw_predictions() - %d valid predictions", valid_count);
            g_sdk->log_console(msg);
            last_draw_log = current_time;
        }

        // Draw each spell slot's prediction if it's recent enough
        for (int i = 0; i < 4; ++i)
        {
            const auto& pred = g_recent_predictions[i];

            if (!pred.is_valid)
                continue;

            // Check if prediction is too old
            float age = current_time - pred.timestamp;
            if (age > settings.max_age_seconds)
            {
                static bool logged_age_expire = false;
                if (!logged_age_expire && g_sdk)
                {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "[PredVisuals] Prediction slot %d expired (age=%.3fs, max=%.3fs)",
                        i, age, settings.max_age_seconds);
                    g_sdk->log_console(msg);
                    logged_age_expire = true;
                }
                continue;
            }

            // Debug: Log that we're about to draw
            static int draw_attempt_count = 0;
            if (draw_attempt_count++ < 3 && g_sdk)
            {
                char msg[256];
                snprintf(msg, sizeof(msg), "[PredVisuals] Drawing slot %d (age=%.3fs)", i, age);
                g_sdk->log_console(msg);
            }

            // Draw line from source to predicted position
            if (settings.draw_line)
            {
                // Convert 3D positions to 2D screen coordinates
                math::vector2 screen_start = g_sdk->renderer->world_to_screen(pred.source_position);
                math::vector2 screen_end = g_sdk->renderer->world_to_screen(pred.cast_position);

                // Debug: Log screen positions (once)
                static bool logged_screen_pos = false;
                if (!logged_screen_pos && g_sdk)
                {
                    char msg[512];
                    snprintf(msg, sizeof(msg), "[PredVisuals] Screen coords: start=(%.0f,%.0f) end=(%.0f,%.0f) | 3D: src=(%.0f,%.0f,%.0f) dst=(%.0f,%.0f,%.0f)",
                        screen_start.x, screen_start.y, screen_end.x, screen_end.y,
                        pred.source_position.x, pred.source_position.y, pred.source_position.z,
                        pred.cast_position.x, pred.cast_position.y, pred.cast_position.z);
                    g_sdk->log_console(msg);
                    logged_screen_pos = true;
                }

                // Check if both positions are valid (not zero/off-screen)
                bool start_valid = (screen_start.x != 0.f || screen_start.y != 0.f);
                bool end_valid = (screen_end.x != 0.f || screen_end.y != 0.f);

                if (start_valid && end_valid)
                {
                    g_sdk->renderer->add_line_2d(
                        screen_start,
                        screen_end,
                        settings.line_thickness,
                        settings.line_color
                    );

                    // Debug: Confirm draw call was made
                    static bool logged_draw = false;
                    if (!logged_draw && g_sdk)
                    {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "[PredVisuals] add_line_2d() called! thickness=%.1f color=0x%08X",
                            settings.line_thickness, settings.line_color);
                        g_sdk->log_console(msg);
                        logged_draw = true;
                    }
                }
                else if (g_sdk)
                {
                    static bool logged_invalid = false;
                    if (!logged_invalid)
                    {
                        g_sdk->log_console("[PredVisuals] Screen positions invalid - not drawing line");
                        logged_invalid = true;
                    }
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

                // Debug: Confirm circle draw call was made
                static bool logged_circle = false;
                if (!logged_circle && g_sdk)
                {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "[PredVisuals] add_circle_3d() called! pos=(%.0f,%.0f,%.0f) radius=%.0f color=0x%08X",
                        pred.cast_position.x, pred.cast_position.y, pred.cast_position.z,
                        pred.spell_radius, settings.circle_color);
                    g_sdk->log_console(msg);
                    logged_circle = true;
                }
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
