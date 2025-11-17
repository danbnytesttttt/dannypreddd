#include "sdk.hpp"
#include "CustomPredictionSDK.h"
#include "PredictionSettings.h"
#include "PredictionTelemetry.h"
#include "PredictionVisuals.h"
#include "FogOfWarTracker.h"

CustomPredictionSDK customPrediction;

// Global variable for champion name
std::string MyHeroNamePredCore;

// Telemetry session tracking
float g_session_start_time = 0.f;

// Menu instance
menu_category* prediction_menu = nullptr;

// Update callback function
void __fastcall on_update()
{
    CustomPredictionSDK::update_trackers();

    // FORCE-SET the SDK pointer every frame (in case platform overwrites it)
    if (sdk::prediction != &customPrediction)
    {
        sdk::prediction = &customPrediction;
    }

    // Verify plugin is active and prediction SDK is accessible (only log if there's a problem)
    static float last_check_time = 0.f;
    float current_time = g_sdk->clock_facade->get_game_time();
    if (current_time - last_check_time >= 10.0f)
    {
        // Check if sdk::prediction points to our implementation
        if (sdk::prediction != &customPrediction)
        {
            g_sdk->log_console("[Danny.Prediction] WARNING: SDK pointer mismatch! Forcing reset...");
            sdk::prediction = &customPrediction;
        }
        last_check_time = current_time;
    }
}

void __fastcall on_draw()
{
    // Draw continuous prediction visualization for current target
    float current_time = g_sdk->clock_facade->get_game_time();
    PredictionVisuals::draw_continuous_prediction(current_time);
}

namespace Prediction
{
    void LoadMenu()
    {
        if (!g_sdk || !g_sdk->menu_manager)
        {
            g_sdk->log_console("[Danny.Prediction] WARNING: Menu manager not available");
            return;
        }

        // Create main prediction menu category
        prediction_menu = g_sdk->menu_manager->add_category("danny_prediction", "Danny Prediction");

        if (prediction_menu)
        {
            // Info label
            prediction_menu->add_label("Danny Prediction v1.0 - Hybrid Physics + Behavior");
            prediction_menu->add_separator();

            // Dash Prediction Toggle
            prediction_menu->add_checkbox(
                "enable_dash_prediction",
                "Enable Dash Prediction",
                true,
                [](bool value) {
                    PredictionSettings::get().enable_dash_prediction = value;
                }
            );

            // Fog of War Prediction Toggle
            prediction_menu->add_checkbox(
                "enable_fog_predictions",
                "Allow Predictions Into Fog",
                false,  // Disabled by default for safety
                [](bool value) {
                    FogOfWarTracker::FogSettings::get().enable_fog_predictions = value;
                }
            );

            // Fog Prediction Time Limit
            prediction_menu->add_slider_float(
                "fog_prediction_time",
                "Max Fog Prediction Time (seconds)",
                0.25f, 2.0f, 0.25f, 0.5f,
                [](float value) {
                    FogOfWarTracker::FogSettings::get().max_fog_prediction_time = value;
                }
            );

            prediction_menu->add_label("Fog predictions use reduced confidence (50%)");

            prediction_menu->add_separator();

            // Debug Logging Toggle
            prediction_menu->add_checkbox(
                "enable_debug_logging",
                "Enable Debug Logging",
                false,
                [](bool value) {
                    PredictionSettings::get().enable_debug_logging = value;
                }
            );

            // Telemetry Toggle
            prediction_menu->add_checkbox(
                "enable_telemetry",
                "Enable Telemetry",
                true,
                [](bool value) {
                    PredictionSettings::get().enable_telemetry = value;
                }
            );

            // Telemetry Output Hotkey
            prediction_menu->add_hotkey(
                "output_telemetry_key",
                "Output Telemetry Now (Hotkey)",
                0,  // No default key - user must bind
                false,  // Not active by default
                false,  // Not a toggle - one-shot trigger
                [](std::string* element_name, bool is_active) {
                    if (is_active && PredictionSettings::get().enable_telemetry)
                    {
                        float session_duration = g_sdk->clock_facade->get_game_time() - g_session_start_time;
                        PredictionTelemetry::TelemetryLogger::finalize(session_duration);
                    }
                    else if (is_active && !PredictionSettings::get().enable_telemetry)
                    {
                        g_sdk->log_console("[Danny.Prediction] Telemetry is disabled - enable it first!");
                    }
                }
            );

            prediction_menu->add_label("Bind a key above to output telemetry on demand");

            prediction_menu->add_separator();

            // === VISUALS SECTION ===
            prediction_menu->add_label("Prediction Visuals (50%+ HitChance Only)");

            // Enable/Disable Visuals
            prediction_menu->add_checkbox(
                "enable_prediction_visuals",
                "Enable Prediction Visuals",
                true,
                [](bool value) {
                    PredictionVisuals::VisualsSettings::get().enabled = value;
                }
            );

            // Color Picker for all visuals
            prediction_menu->add_colorpicker(
                "prediction_color",
                "Prediction Color",
                0xFFE19D9D,  // Default salmon/pink
                [](uint32_t color) {
                    PredictionVisuals::VisualsSettings::get().main_color = color;
                }
            );

            // Draw Current Position
            prediction_menu->add_checkbox(
                "draw_current_position",
                "Show Current Position (Light)",
                true,
                [](bool value) {
                    PredictionVisuals::VisualsSettings::get().draw_current_position = value;
                }
            );

            // Draw Predicted Position
            prediction_menu->add_checkbox(
                "draw_predicted_position",
                "Show Predicted Position",
                true,
                [](bool value) {
                    PredictionVisuals::VisualsSettings::get().draw_predicted_position = value;
                }
            );

            // Draw Movement Line
            prediction_menu->add_checkbox(
                "draw_movement_line",
                "Show Skillshot Line",
                true,
                [](bool value) {
                    PredictionVisuals::VisualsSettings::get().draw_movement_line = value;
                }
            );

            // Prediction Time
            prediction_menu->add_slider_float(
                "prediction_time",
                "Prediction Time (seconds)",
                0.25f, 2.0f, 0.25f, 0.75f,
                [](float value) {
                    PredictionVisuals::VisualsSettings::get().prediction_time = value;
                }
            );

            prediction_menu->add_label("Current (light) + Predicted + Line from you");

            prediction_menu->add_separator();

            // Grid Search Quality
            std::vector<std::string> quality_options = {
                "Performance (8x8)",
                "Balanced (10x10)",
                "Quality (12x12)",
                "Maximum (16x16)"
            };

            prediction_menu->add_combo(
                "grid_search_quality",
                "Prediction Quality",
                quality_options,
                0,  // Default: Performance
                [](int value) {
                    switch (value)
                    {
                        case 0: PredictionSettings::get().grid_search_resolution = 8; break;
                        case 1: PredictionSettings::get().grid_search_resolution = 10; break;
                        case 2: PredictionSettings::get().grid_search_resolution = 12; break;
                        case 3: PredictionSettings::get().grid_search_resolution = 16; break;
                        default: PredictionSettings::get().grid_search_resolution = 8; break;
                    }
                }
            );

            g_sdk->log_console("[Danny.Prediction] Menu created successfully");
        }
        else
        {
            g_sdk->log_console("[Danny.Prediction] WARNING: Failed to create menu category!");
        }
    }

    void LoadPrediction()
    {
        // Version check - this forces recompile and helps verify new code is loaded
        g_sdk->log_console("=== DANNY PREDICTION v1.3 - FIXED DRAWING EVENT ===");

        // Register update callback for tracker updates
        g_sdk->event_manager->register_callback(event_manager::event::game_update, reinterpret_cast<void*>(on_update));

        // Register draw callback for visualization rendering
        g_sdk->event_manager->register_callback(event_manager::event::draw_world, reinterpret_cast<void*>(on_draw));

        // Create menu
        LoadMenu();

        // Initialize telemetry
        g_session_start_time = g_sdk->clock_facade->get_game_time();
        if (PredictionSettings::get().enable_telemetry)
        {
            PredictionTelemetry::TelemetryLogger::initialize(MyHeroNamePredCore, true);
            g_sdk->log_console("[Danny.Prediction] Telemetry enabled - will output to console at game end");
        }
    }

    void UnloadPrediction()
    {
        g_sdk->log_console("[Danny.Prediction] UnloadPrediction() called - game ending");

        // Finalize telemetry
        if (PredictionSettings::get().enable_telemetry)
        {
            g_sdk->log_console("[Danny.Prediction] Finalizing telemetry...");
            float session_duration = g_sdk->clock_facade->get_game_time() - g_session_start_time;
            PredictionTelemetry::TelemetryLogger::finalize(session_duration);
            g_sdk->log_console("[Danny.Prediction] Telemetry finalized - check console output above");
        }
        else
        {
            g_sdk->log_console("[Danny.Prediction] Telemetry was disabled - no report generated");
        }

        // Unregister callbacks
        g_sdk->event_manager->unregister_callback(event_manager::event::game_update, reinterpret_cast<void*>(on_update));
        g_sdk->event_manager->unregister_callback(event_manager::event::draw_world, reinterpret_cast<void*>(on_draw));

        // Clean up prediction visuals
        PredictionVisuals::clear();

        // Clean up fog of war tracker
        FogOfWarTracker::clear();

        // Clean up all trackers
        HybridPred::PredictionManager::clear();

        // Menu is automatically cleaned up by SDK
    }
}

// Name export - makes it appear in Prediction dropdown instead of as toggle
// Following naming convention: AuthorName.Prediction
extern "C" __declspec(dllexport) const char* Name = "Danny.Prediction";

extern "C" __declspec(dllexport) int SDKVersion = SDK_VERSION;
extern "C" __declspec(dllexport) module_type Type = module_type::pred;

extern "C" __declspec(dllexport) bool PluginLoad(core_sdk* sdk, void** custom_sdk)
{
    g_sdk = sdk;

    // CRITICAL: Log plugin load attempt
    g_sdk->log_console("==============================================");
    g_sdk->log_console("[Danny.Prediction] Plugin loading...");

    if (!sdk_init::target_selector())
    {
        g_sdk->log_console("[Danny.Prediction] ERROR: Target selector init failed!");
        return false;
    }

    *custom_sdk = &customPrediction;

    MyHeroNamePredCore = g_sdk->object_manager->get_local_player()->get_char_name();

    Prediction::LoadPrediction();

    // CRITICAL: Set the global prediction pointer to our implementation
    char ptr_msg[256];
    sprintf_s(ptr_msg, sizeof(ptr_msg), "[Danny.Prediction] Before: sdk::prediction = 0x%p, &customPrediction = 0x%p",
        sdk::prediction, &customPrediction);
    g_sdk->log_console(ptr_msg);

    sdk::prediction = &customPrediction;

    // Verify it was set correctly
    sprintf_s(ptr_msg, sizeof(ptr_msg), "[Danny.Prediction] After:  sdk::prediction = 0x%p, &customPrediction = 0x%p",
        sdk::prediction, &customPrediction);
    g_sdk->log_console(ptr_msg);

    if (sdk::prediction == &customPrediction)
    {
        g_sdk->log_console("[Danny.Prediction] Global SDK pointer successfully set!");
    }
    else
    {
        g_sdk->log_console("[Danny.Prediction] ERROR: Failed to set global SDK pointer!");
    }

    // Confirm successful load
    char load_msg[256];
    sprintf_s(load_msg, sizeof(load_msg), "[Danny.Prediction] Successfully loaded for champion: %s", MyHeroNamePredCore.c_str());
    g_sdk->log_console(load_msg);
    g_sdk->log_console("[Danny.Prediction] SDK pointer registered - ready for predictions!");
    g_sdk->log_console("==============================================");

    return true;
}

extern "C" __declspec(dllexport) void PluginUnload()
{
    g_sdk->log_console("[Danny.Prediction] Plugin unloading...");
    Prediction::UnloadPrediction();
    g_sdk->log_console("[Danny.Prediction] Plugin unloaded successfully.");
}