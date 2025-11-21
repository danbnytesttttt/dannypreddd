# Vector Spells Guide (Viktor E, Rumble R, Irelia E)

## Overview
Vector spells require **TWO cast positions** - a start point and an end point. The prediction SDK properly handles this, but you must configure the spell data correctly AND use both positions when casting.

## Issue Analysis

After reviewing the codebase, the vector spell implementation is **working correctly**. The most common reasons Viktor E doesn't cast are:

### 1. **Incorrect spell_type** ❌
```cpp
// WRONG - Using linear instead of vector
pred_sdk::spell_data data;
data.spell_type = pred_sdk::spell_type::linear;  // ❌ Viktor E won't work!
```

```cpp
// CORRECT
pred_sdk::spell_data data;
data.spell_type = pred_sdk::spell_type::vector;  // ✅
```

### 2. **Missing cast_range** ❌
```cpp
// WRONG - Not setting cast_range
pred_sdk::spell_data data;
data.spell_type = pred_sdk::spell_type::vector;
data.range = 1225.f;     // Viktor E laser length
// Missing: data.cast_range = 525.f;  // ❌ Initial cast range!
```

```cpp
// CORRECT
pred_sdk::spell_data data;
data.spell_type = pred_sdk::spell_type::vector;
data.range = 1225.f;        // Laser line length
data.cast_range = 525.f;    // Max distance for first cast ✅
data.radius = 80.f;         // Laser width
```

### 3. **Not using both cast positions** ❌
```cpp
// WRONG - Only using cast_position
auto result = prediction->predict(spell_data);
if (result.is_valid && result.hitchance >= spell_data.expected_hitchance)
{
    player->cast_spell(spell_slot, result.cast_position);  // ❌ Missing first position!
}
```

```cpp
// CORRECT - Using BOTH positions for vector spells
auto result = prediction->predict(spell_data);
if (result.is_valid && result.hitchance >= spell_data.expected_hitchance)
{
    // For vector spells, use the two-position overload
    player->cast_spell(spell_slot, result.first_cast_position, result.cast_position);  // ✅
}
```

## Complete Viktor E Example

```cpp
// Viktor E Spell Configuration
pred_sdk::spell_data viktor_e;

// CRITICAL: Set spell_type to vector
viktor_e.spell_type = pred_sdk::spell_type::vector;

// Spell parameters
viktor_e.source = player;
viktor_e.range = 1225.f;           // Laser line length (from first to second cast)
viktor_e.cast_range = 525.f;       // Maximum distance for initial cast point
viktor_e.radius = 80.f;            // Laser width (half-width, so total is 160)
viktor_e.delay = 0.f;              // Viktor E has no delay
viktor_e.projectile_speed = 1200.f; // Laser travel speed
viktor_e.spell_slot = spell_slot::e;
viktor_e.expected_hitchance = pred_sdk::hitchance::medium;

// Targeting type (center to edge is standard for skillshots)
viktor_e.targetting_type = pred_sdk::targetting_type::center_to_edge;

// Get prediction
auto result = sdk::prediction->predict(viktor_e);

// Cast if valid
if (result.is_valid && result.hitchance >= viktor_e.expected_hitchance)
{
    // MUST use two-position cast for vector spells!
    player->cast_spell(spell_slot::e, result.first_cast_position, result.cast_position);
}
```

## How Vector Prediction Works

The prediction system:

1. **Computes optimal laser orientation** - Tests 20 different angles to find the best line placement
2. **Sets first_cast_position** - The starting point of the laser (within cast_range of player)
3. **Sets cast_position** - The ending point of the laser (range distance from first_cast)
4. **Validates cast_range constraint** - Ensures first_cast is reachable

### Visual Representation
```
         Player (Source)
            |
            |--- (cast_range = 525) --->  First Cast Position
                                               |
                                               |--- (range = 1225) --->  Cast Position
                                               |                              |
                                               <------- Laser Line ---------->
                                               (This is what hits the target)
```

## Other Vector Spells

### Rumble R (The Equalizer)
```cpp
pred_sdk::spell_data rumble_r;
rumble_r.spell_type = pred_sdk::spell_type::vector;
rumble_r.range = 1000.f;       // Flame length
rumble_r.cast_range = 1700.f;  // Initial cast range
rumble_r.radius = 200.f;       // Flame width
rumble_r.delay = 0.5f;
rumble_r.projectile_speed = FLT_MAX;  // Instant damage
```

### Irelia E (Flawless Duet)
```cpp
pred_sdk::spell_data irelia_e;
irelia_e.spell_type = pred_sdk::spell_type::vector;
irelia_e.range = 850.f;        // Max distance between blades
irelia_e.cast_range = 850.f;   // First blade cast range
irelia_e.radius = 90.f;        // Hitbox width
irelia_e.delay = 0.5f;         // Time before stun triggers
irelia_e.projectile_speed = FLT_MAX;
```

## Debugging Checklist

If Viktor E still isn't casting, verify:

- [ ] `spell_type` is set to `pred_sdk::spell_type::vector`
- [ ] `cast_range` is set (not 0 or uninitialized)
- [ ] `range` is set (laser line length)
- [ ] `radius` is set (laser width)
- [ ] Using **two-position** `cast_spell()` overload
- [ ] `result.is_valid` is true before casting
- [ ] `result.hitchance` meets your threshold
- [ ] Both `first_cast_position` and `cast_position` have valid coordinates (not all zeros)

## Common Errors

### Error: "Spell doesn't cast at all"
**Cause**: Using single-position cast_spell() instead of two-position version
**Fix**: Use `player->cast_spell(slot, first_cast_position, cast_position)`

### Error: "Laser goes in wrong direction"
**Cause**: Swapping first_cast_position and cast_position
**Fix**: Use positions in correct order (first position first, second position second)

### Error: "first_cast_position is (0,0,0)"
**Cause**: spell_type not set to vector, so prediction uses linear/circular logic
**Fix**: Explicitly set `data.spell_type = pred_sdk::spell_type::vector`

### Error: "Laser is too short or doesn't reach target"
**Cause**: `range` set incorrectly (should be laser length, not cast range)
**Fix**: Viktor E laser travels 1225 units from first_cast to cast_position

---

## SDK Functions Used

```cpp
// Native SDK - Two-position cast for vector spells
void cast_spell(int spell_slot, math::vector3 start_position, math::vector3 end_position);

// Prediction SDK - Returns both positions for vector spells
class pred_data {
    math::vector3 first_cast_position;  // First position (Viktor E start)
    math::vector3 cast_position;        // Second position (Viktor E end)
};
```

## Implementation Details (Technical)

The vector prediction algorithm (`HybridFusionEngine::optimize_vector_orientation`):

1. Tests 20 orientations (0°-360° in 18° increments)
2. For each orientation:
   - Positions laser line centered on predicted target
   - Validates first_cast is within cast_range
   - Computes hit probability using capsule geometry
   - Factors in both physics (reachability) and behavior (movement patterns)
3. Returns configuration with highest hit chance
4. If all orientations fail, uses default (aim at target direction)

**Time Complexity**: O(20) = O(1) per prediction call
**Optimized**: Pre-validated, single-pass algorithm

---

**Need more help?** Check the source code:
- Vector prediction logic: `HybridPrediction.cpp:1920-2021`
- Orientation optimization: `HybridPrediction.cpp:2381-2514`
- Prediction conversion: `CustomPredictionSDK.cpp:294-323`
