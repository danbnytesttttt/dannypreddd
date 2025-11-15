#pragma once

/**
 * =============================================================================
 * PREDICTION CONFIGURATION
 * =============================================================================
 *
 * Global configuration settings for the hybrid prediction system.
 * Accessed via PredictionConfig::get()
 *
 * =============================================================================
 */

namespace PredictionConfig
{
    /**
     * Configuration settings
     */
    struct Settings
    {
        // Edge case toggles
        bool enable_dash_prediction = true;  // Predict at dash endpoints

        Settings() {}
    };

    /**
     * Get global configuration instance
     */
    inline Settings& get()
    {
        static Settings instance;
        return instance;
    }

} // namespace PredictionConfig
