# Hitchance Casting Bug Fix

## Problem
Champion scripts are making low-confidence casts despite setting hitchance to "high" or "very high" in the menu.

**Telemetry Evidence:**
- 48.6% of casts were below 40% hitchance
- Distribution: 0-20: 5 casts | 20-40: 12 casts | 40-60: 3 casts | 60-80: 3 casts | 80-100: 12 casts
- Thresh Q was set to `pred_sdk::hitchance::high` (70) but casting at 15%, 28%, etc.

## Root Cause
The `spell::cast_spell_on_hitchance()` function in `spell.cpp` only checks `pred.is_valid` but does NOT check if the actual hitchance meets the minimum threshold.

**Buggy Code (spell.cpp:454):**
```cpp
bool spell::cast_spell_on_hitchance(game_object* target, const int hitchance,
    const std::optional<pred_input_options>& options, const float t)
{
    const auto& player = g_sdk->object_manager->get_local_player();
    const auto& time = g_sdk->clock_facade->get_game_time();
    const auto& pred = this->get_prediction(target, hitchance, options);

    if (pred.is_valid)  // ❌ BUG: Not checking hitchance threshold!
    {
        return this->cast_spell(pred.cast_position, t);
    }

    return false;
}
```

## Fix
Add hitchance comparison to the condition:

**Fixed Code:**
```cpp
bool spell::cast_spell_on_hitchance(game_object* target, const int hitchance,
    const std::optional<pred_input_options>& options, const float t)
{
    const auto& player = g_sdk->object_manager->get_local_player();
    const auto& time = g_sdk->clock_facade->get_game_time();
    const auto& pred = this->get_prediction(target, hitchance, options);

    if (pred.is_valid && pred.hitchance >= hitchance)  // ✅ FIX: Check both validity AND hitchance!
    {
        return this->cast_spell(pred.cast_position, t);
    }

    return false;
}
```

## Impact
After this fix:
- When Q is set to "high" (70), it will ONLY cast when prediction hitchance >= 70
- When Q is set to "very high" (85), it will ONLY cast when prediction hitchance >= 85
- No more low-confidence casts (15%, 28%, etc.)

## Location
This fix needs to be applied in your champion script framework's `spell.cpp` file, NOT in the dannypreddd prediction SDK repository.
