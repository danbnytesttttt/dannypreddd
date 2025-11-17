#include "sdk.hpp"
#include "CustomPredictionSDK.h"
#include "PredictionSettings.h"
#include "PredictionTelemetry.h"

CustomPredictionSDK customPrediction;

// Global variable for champion name
std::string MyHeroNamePredCore;

// Telemetry session tracking
float g_session_start_time = 0.f;

// Menu instances
namespace menu_sdk = sdk::menu;
menu_sdk::menu_item* prediction_menu = nullptr;

// Menu items
menu_sdk::menu_item* enable_dash_prediction_item = nullptr;
menu_sdk::menu_item* enable_debug_logging_item = nullptr;
menu_sdk::menu_item* enable_telemetry_item = nullptr;
menu_sdk::menu_item* grid_search_quality_item = nullptr;

// Menu callback to update settings
void update_menu_settings()
{
    auto& settings = PredictionSettings::get();

    if (enable_dash_prediction_item)
        settings.enable_dash_prediction = enable_dash_prediction_item->get_bool();

    if (enable_debug_logging_item)
        settings.enable_debug_logging = enable_debug_logging_item->get_bool();

    if (enable_telemetry_item)
        settings.enable_telemetry = enable_telemetry_item->get_bool();

    if (grid_search_quality_item)
    {
        int quality = grid_search_quality_item->get_int();
        // 0 = Performance (8), 1 = Balanced (10), 2 = Quality (12), 3 = Maximum (16)
        switch (quality)
        {
            case 0: settings.grid_search_resolution = 8; break;
            case 1: settings.grid_search_resolution = 10; break;
            case 2: settings.grid_search_resolution = 12; break;
            case 3: settings.grid_search_resolution = 16; break;
            default: settings.grid_search_resolution = 8; break;
        }
    }
}

// Update callback function
void __fastcall on_update()
{
    CustomPredictionSDK::update_trackers();

    // Update settings from menu (only do this occasionally to avoid overhead)
    static float last_menu_update = 0.f;
    float current_time = g_sdk->clock_facade->get_game_time();
    if (current_time - last_menu_update >= 0.5f)  // Update every 500ms
    {
        update_menu_settings();
        last_menu_update = current_time;
    }

    // FORCE-SET the SDK pointer every frame (in case platform overwrites it)
    if (sdk::prediction != &customPrediction)
    {
        sdk::prediction = &customPrediction;
    }

    // Verify plugin is active and prediction SDK is accessible
    static float last_check_time = 0.f;
    if (current_time - last_check_time >= 10.0f)
    {
        // Check if sdk::prediction points to our implementation
        if (sdk::prediction == &customPrediction)
        {
            g_sdk->log_console("[Danny.Prediction] ACTIVE - SDK pointer confirmed");
        }
        else
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
        // Create main prediction menu
        prediction_menu = g_sdk->menu_manager->create_menu("danny_prediction", "Danny Prediction");

        if (prediction_menu)
        {
            // Dash Prediction Toggle
            enable_dash_prediction_item = prediction_menu->add_checkbox(
                "enable_dash_prediction",
                "Enable Dash Prediction",
                true
            );
            enable_dash_prediction_item->set_tooltip("Predict enemy position at dash endpoint (disable if causing issues)");

            // Debug Logging Toggle
            enable_debug_logging_item = prediction_menu->add_checkbox(
                "enable_debug_logging",
                "Enable Debug Logging",
                false
            );
            enable_debug_logging_item->set_tooltip("Enable verbose console logging for debugging (may impact performance)");

            // Telemetry Toggle
            enable_telemetry_item = prediction_menu->add_checkbox(
                "enable_telemetry",
                "Enable Telemetry",
                true
            );
            enable_telemetry_item->set_tooltip(
                "Log prediction data to file for post-game analysis\n"
                "File: dannypred_telemetry_TIMESTAMP.txt\n"
                "Send this file for performance analysis"
            );

            // Grid Search Quality
            grid_search_quality_item = prediction_menu->add_combobox(
                "grid_search_quality",
                "Prediction Quality",
                { "Performance (Fast)", "Balanced", "Quality", "Maximum (Slow)" },
                0  // Default: Performance
            );
            grid_search_quality_item->set_tooltip(
                "Grid search resolution for circular spells:\n"
                "Performance: 8x8 grid (fastest)\n"
                "Balanced: 10x10 grid\n"
                "Quality: 12x12 grid\n"
                "Maximum: 16x16 grid (slowest, best quality)"
            );

            // Separator
            prediction_menu->add_separator();

            // Info text
            auto info = prediction_menu->add_text("info", "Danny Prediction v1.0 - Hybrid Physics + Behavior");
            info->set_color(color(100, 200, 255, 255));

            g_sdk->log_console("[Danny.Prediction] Menu created successfully");
        }
        else
        {
            g_sdk->log_console("[Danny.Prediction] WARNING: Failed to create menu!");
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
    sprintf_s(ptr_msg, "[Danny.Prediction] Before: sdk::prediction = 0x%p, &customPrediction = 0x%p",
        sdk::prediction, &customPrediction);
    g_sdk->log_console(ptr_msg);

    sdk::prediction = &customPrediction;

    // Verify it was set correctly
    sprintf_s(ptr_msg, "[Danny.Prediction] After:  sdk::prediction = 0x%p, &customPrediction = 0x%p",
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
    sprintf_s(load_msg, "[Danny.Prediction] Successfully loaded for champion: %s", MyHeroNamePredCore.c_str());
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