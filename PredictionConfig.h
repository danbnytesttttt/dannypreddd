#pragma once

// DEPRECATED: Use PredictionSettings.h instead
// This file is kept for backwards compatibility
#include "PredictionSettings.h"

namespace PredictionConfig
{
    /**
     * Get global configuration instance (forwards to PredictionSettings)
     */
    inline PredictionSettings::Settings& get()
    {
        return PredictionSettings::get();
    }

} // namespace PredictionConfig
