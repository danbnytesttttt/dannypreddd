#include "sdk.hpp"
#include "CustomPredictionSDK.h"

CustomPredictionSDK customPrediction;

// Global variable for champion name
std::string MyHeroNamePredCore;

// Update callback function
void __fastcall on_update()
{
    CustomPredictionSDK::update_trackers();
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