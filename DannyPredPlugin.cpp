#include "sdk.hpp"
#include "CustomPredictionSDK.h"
#include "PredictionVisuals.h"
#include "PredictionTelemetry.h"
#include "FogOfWarTracker.h"

CustomPredictionSDK customPrediction;

// Global variable for champion name
std::string MyHeroNamePredCore;

// Update callback function
void __fastcall on_update()
{
    CustomPredictionSDK::update_trackers();
}

// Render callback function for visual indicators
void __fastcall on_draw()
{
    if (!g_sdk || !g_sdk->clock_facade)
        return;

    float current_time = g_sdk->clock_facade->get_game_time();
    PredictionVisuals::draw_continuous_prediction(current_time);
}

namespace Prediction
{
    void LoadPrediction()
    {
        // Register update callback for tracker updates
        g_sdk->event_manager->register_callback(event_manager::event::game_update, reinterpret_cast<void*>(on_update));

        // Register draw callback for visual indicators
        g_sdk->event_manager->register_callback(event_manager::event::draw, reinterpret_cast<void*>(on_draw));

        if (PredictionSettings::get().enable_debug_logging)
            g_sdk->log_console("[Danny.Prediction] Loaded - visuals and trackers initialized");
    }

    void UnloadPrediction()
    {
        // Write telemetry report before unloading
        if (PredictionSettings::get().enable_telemetry)
        {
            g_sdk->log_console("[Danny.Prediction] ===== SESSION TELEMETRY REPORT =====");
            PredictionTelemetry::TelemetryLogger::write_report();
        }

        // Unregister callbacks
        g_sdk->event_manager->unregister_callback(event_manager::event::game_update, reinterpret_cast<void*>(on_update));
        g_sdk->event_manager->unregister_callback(event_manager::event::draw, reinterpret_cast<void*>(on_draw));

        // Clean up all subsystems
        HybridPred::PredictionManager::clear();
        FogOfWarTracker::clear();
        PredictionVisuals::clear();

        if (PredictionSettings::get().enable_debug_logging)
            g_sdk->log_console("[Danny.Prediction] Unloaded - all subsystems cleared");
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

    if (!sdk_init::target_selector())
    {
        return false;
    }

    *custom_sdk = &customPrediction;

    // CRITICAL: Validate object_manager and local player before accessing
    if (!g_sdk->object_manager || !g_sdk->object_manager->get_local_player())
    {
        return false;
    }

    MyHeroNamePredCore = g_sdk->object_manager->get_local_player()->get_char_name();

    Prediction::LoadPrediction();

    return true;
}

extern "C" __declspec(dllexport) void PluginUnload()
{
    Prediction::UnloadPrediction();
}