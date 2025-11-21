#include "sdk.hpp"
#include "CustomPredictionSDK.h"

CustomPredictionSDK customPrediction;

// Global variable for champion name
std::string MyHeroNamePredCore;

// Menu category pointer
menu_category* g_menu = nullptr;

// Settings
namespace PredSettings
{
    bool debug_logs = false;
    bool show_predictions = false;
}

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

        // Create menu category
        g_menu = g_sdk->menu_manager->add_category("danny_prediction", "Danny.Prediction");

        if (g_menu)
        {
            g_menu->add_label("Danny.Prediction Settings");

            g_menu->add_checkbox("debug_logs", "Show Debug Logs", false, [](bool value) {
                PredSettings::debug_logs = value;
            });

            g_menu->add_checkbox("show_predictions", "Draw Predictions", false, [](bool value) {
                PredSettings::show_predictions = value;
            });
        }
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

    if (!sdk_init::target_selector())
    {
        return false;
    }

    *custom_sdk = &customPrediction;

    MyHeroNamePredCore = g_sdk->object_manager->get_local_player()->get_char_name();

    Prediction::LoadPrediction();

    return true;
}

extern "C" __declspec(dllexport) void PluginUnload()
{
    Prediction::UnloadPrediction();
}