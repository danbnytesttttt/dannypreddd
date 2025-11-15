#include "sdk.hpp"
#include "CustomPredictionSDK.h"

CustomPredictionSDK customPrediction;

// Global variable for champion name
std::string MyHeroNamePredCore;

// Update callback function
void __fastcall on_update()
{
    CustomPredictionSDK::update_trackers();

    // Verify plugin is active and prediction SDK is accessible
    static float last_check_time = 0.f;
    float current_time = g_sdk->clock_facade->get_game_time();
    if (current_time - last_check_time >= 10.0f)
    {
        // Check if sdk::prediction points to our implementation
        if (sdk::prediction == &customPrediction)
        {
            g_sdk->log_console("[Danny.Prediction] ACTIVE - SDK pointer confirmed");
        }
        else
        {
            g_sdk->log_console("[Danny.Prediction] WARNING: SDK pointer mismatch! Our prediction is NOT being used!");
        }
        last_check_time = current_time;
    }
}

namespace Prediction
{
    void LoadPrediction()
    {
        // Register update callback for tracker updates
        g_sdk->event_manager->register_callback(event_manager::event::game_update, reinterpret_cast<void*>(on_update));
    }

    void UnloadPrediction()
    {
        // Unregister callback
        g_sdk->event_manager->unregister_callback(event_manager::event::game_update, reinterpret_cast<void*>(on_update));

        // Clean up all trackers
        HybridPred::PredictionManager::clear();
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
    sdk::prediction = &customPrediction;

    // Verify it was set correctly
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