#include "sdk.hpp"
#include "CustomPredictionSDK.h"
#include "PredictionSettings.h"
#include "PredictionTelemetry.h"

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

            prediction_menu->add_label("Telemetry outputs to console at game end");

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
        // Register update callback for tracker updates
        g_sdk->event_manager->register_callback(event_manager::event::game_update, reinterpret_cast<void*>(on_update));

        // Create menu
        LoadMenu();

        // Initialize telemetry
        g_session_start_time = g_sdk->clock_facade->get_game_time();
        if (PredictionSettings::get().enable_telemetry)
        {
            PredictionTelemetry::TelemetryLogger::initialize(MyHeroNamePredCore, true);
            g_sdk->log_console("[Danny.Prediction] Telemetry enabled - logging to file");
        }
    }

    void UnloadPrediction()
    {
        // Finalize telemetry
        if (PredictionSettings::get().enable_telemetry)
        {
            float session_duration = g_sdk->clock_facade->get_game_time() - g_session_start_time;
            PredictionTelemetry::TelemetryLogger::finalize(session_duration);
            g_sdk->log_console("[Danny.Prediction] Telemetry finalized - check log file");
        }

        // Unregister callback
        g_sdk->event_manager->unregister_callback(event_manager::event::game_update, reinterpret_cast<void*>(on_update));

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