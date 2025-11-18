# Deep Dive: Hitchance Calculation & Casting Decision Logic

## Table of Contents
1. [Overview](#overview)
2. [Step-by-Step Pipeline](#step-by-step-pipeline)
3. [Physics Probability (Deterministic)](#physics-probability)
4. [Behavior Probability (Learned Patterns)](#behavior-probability)
5. [Probability Fusion (Hybrid Approach)](#probability-fusion)
6. [Hitchance Enum Conversion](#hitchance-enum-conversion)
7. [Casting Decision Logic](#casting-decision-logic)
8. [Example Walkthrough](#example-walkthrough)

---

## Overview

The prediction system calculates **when to cast a spell** using a **two-component hybrid model**:

```
┌─────────────┐    ┌──────────────┐
│   Physics   │    │   Behavior   │
│ (Can reach) │    │ (Will move)  │
└──────┬──────┘    └──────┬───────┘
       │                  │
       └────────┬─────────┘
                │
         ┌──────▼──────┐
         │    Fusion   │ ← Adaptive weighting
         │  (Hybrid)   │
         └──────┬──────┘
                │
         ┌──────▼──────┐
         │ Hit Chance  │ ← Float [0,1]
         │   (0.75)    │
         └──────┬──────┘
                │
         ┌──────▼──────┐
         │  Hitchance  │ ← Enum conversion
         │    (high)   │
         └──────┬──────┘
                │
         ┌──────▼──────┐
         │Cast Decision│ ← Threshold check
         │  (Cast/Hold)│
         └─────────────┘
```

**Key Concept**: We compute TWO independent probabilities and fuse them:
- **Physics**: "Can the target physically reach the spell area given their movement speed?"
- **Behavior**: "Based on their patterns, will they actually move there?"

---

## Step-by-Step Pipeline

### 1. Compute Arrival Time
*Location: `HybridPrediction.cpp:990-1005`*

```cpp
float arrival_time = delay + (distance / projectile_speed)
```

**Example**: Xerath Q
- Distance: 800 units
- Projectile speed: 500 u/s
- Delay: 0.5s
- **Arrival time** = 0.5s + (800/500) = **2.1 seconds**

---

### 2. Build Reachable Region (Physics)
*Location: `HybridPrediction.cpp:895-962`*

Computes a **circle** representing all positions the target could physically reach.

#### Formula:
```
max_radius = v₀ * t + 0.5 * a * t²  (during acceleration)
           + v_max * (t - t_accel)   (at max speed)
```

#### Variables:
- `v₀` = current velocity magnitude
- `v_max` = target's movement speed (e.g., 350 units/s)
- `a` = acceleration (1200 units/s²)
- `t` = arrival time

#### Example:
```cpp
// Target stats
current_velocity = (100, 0, 100)  // magnitude = 141 u/s
move_speed = 350 u/s
arrival_time = 2.1s

// Already moving at 141 u/s, needs to accelerate to 350 u/s
speed_diff = 350 - 141 = 209 u/s
accel_time = 209 / 1200 = 0.174s

// Distance during acceleration
accel_distance = 141 * 0.174 + 0.5 * 1200 * 0.174² = 42.7 units

// Distance at max speed
max_speed_time = 2.1 - 0.174 = 1.926s
max_speed_distance = 350 * 1.926 = 674.1 units

// Total reachable distance
max_radius = 42.7 + 674.1 = 716.8 units
```

**Result**: Circle centered at target's **current position** with radius **716.8 units**.

**Area**: π × 716.8² = **1,613,770 square units**

---

### 3. Build Behavior PDF (Learned Patterns)
*Location: `HybridPrediction.cpp:1070-1106`*

Constructs a **probability density function** from movement history.

#### Data Structure:
```cpp
struct BehaviorPDF {
    float pdf_grid[32][32];     // 32×32 grid
    float cell_size = 25.0f;    // Each cell is 25×25 units
    math::vector3 origin;       // Grid center
};
```

#### How it's built:
1. **Sample movement history** (last 100 positions)
2. **Apply exponential decay** weighting (recent = more important)
3. **Add weighted samples to grid** using Gaussian kernel
4. **Normalize** so total probability = 1.0

#### Example:
```
Target's recent movement (last 2 seconds):
- 70% forward movement
- 20% side-dodge left
- 10% side-dodge right

Behavioral PDF (heatmap):
       Forward
         ★★★★★  (0.7)
     ★★         (0.2)
       ★        (0.1)
   Left   Right
```

**Interpretation**: If we aim at the "forward" cluster, behavior prob = 0.7 (70% of mass).

---

### 4. Compute Physics Probability
*Location: `HybridPrediction.cpp:972-988`*

Calculates: **"If target is equally likely to move anywhere in reachable circle, what's the probability they're in our spell area?"**

#### Formula:
```
P_physics = intersection_area / reachable_area
```

#### Geometry:
```
     Reachable Circle           Spell Circle
     (radius=716.8)            (radius=100)
         ╭────╮
       ╭─╯░░░░╰─╮
      ╱  ░░╔══╗░░╲
     │   ░░║██║░░  │
     │   ░░╚══╝░░  │   ← Intersection area
      ╲  ░░░░░░░░╱
       ╰─╮░░░░╭─╯
         ╰────╯
```

#### Calculation:
```cpp
intersection_area = circle_circle_intersection(
    spell_center, spell_radius=100,
    reachable_center, reachable_radius=716.8
)

// Using lens formula for circle-circle intersection
intersection_area = 31,416 square units  // π × 100²

P_physics = 31,416 / 1,613,770 = 0.0195 (1.95%)
```

**Low physics probability!** Why? The reachable area is HUGE compared to our spell.

---

### 5. Compute Behavior Probability
*Location: `HybridPrediction.cpp:1070-1106`*

Calculates: **"Based on learned patterns, what's the probability they move into our spell area?"**

#### Method:
```cpp
prob = 0.0
for each cell in 32×32 grid:
    cell_world_position = compute_world_pos(x, z)
    if distance(cell_world_position, spell_center) <= spell_radius:
        prob += pdf_grid[x][z]  // Sum probability mass

return prob  // Already [0,1] because PDF is normalized
```

#### Example:
```
Spell aimed at "forward" cluster:
   ★★★★★  ← Our spell circle covers this area
     ★★
       ★

Cells inside spell circle:
- Cell (16, 20): 0.35
- Cell (16, 21): 0.30
- Cell (15, 20): 0.05

P_behavior = 0.35 + 0.30 + 0.05 = 0.70 (70%)
```

**High behavior probability!** The target's patterns indicate they're likely to move forward.

---

### 6. Probability Fusion (The Key Innovation!)
*Location: `HybridPrediction.h:114-151`*

Combines physics and behavior using **adaptive weighted geometric mean**.

#### Formula:
```
HitChance = (P_physics^w) × (P_behavior^(1-w)) × Confidence
```

Where `w` = physics weight (0-1)

#### Adaptive Weighting:

| Sample Count | Physics Weight | Behavior Weight | Rationale |
|--------------|----------------|-----------------|-----------|
| 0-10 | 0.7 → 0.5 | 0.3 → 0.5 | Sparse data: trust physics |
| 10-20 | 0.5 → 0.3 | 0.5 → 0.7 | Transitioning |
| 20+ | 0.3 | 0.7 | Abundant data: trust behavior |

#### Staleness Detection (NEW):
```cpp
if (time_since_update > 0.5s):
    // Data is stale (fog, lag, etc.)
    physics_weight += min(time_since_update - 0.5, 1.0) * 0.3
    physics_weight = min(physics_weight, 0.8)  // Cap at 0.8
```

#### Example Calculation:
```cpp
P_physics = 0.0195 (1.95%)
P_behavior = 0.70 (70%)
sample_count = 45  // Abundant data
time_since_update = 0.1s  // Fresh data

// Determine weight
physics_weight = 0.3  // Abundant samples → trust behavior more
behavior_weight = 0.7

// No staleness penalty (updated 0.1s ago)

// Fusion
fused = (0.0195^0.3) × (0.70^0.7) × confidence
      = 0.305 × 0.767 × 0.85  // confidence from distance/latency
      = 0.199 (19.9%)
```

**Why geometric mean?**
- Multiplicative: If EITHER component is low, result is low
- Prevents "false confidence" from high behavior when physics says impossible
- More robust than arithmetic mean

---

### 7. Hitchance Enum Conversion
*Location: `CustomPredictionSDK.cpp:325-341`*

Converts continuous probability [0,1] to discrete enum.

```cpp
if (hit_chance >= 0.95)  → guaranteed_hit (100)
if (hit_chance >= 0.80)  → very_high      (85)
if (hit_chance >= 0.65)  → high           (70)
if (hit_chance >= 0.50)  → medium         (50)
if (hit_chance >= 0.30)  → low            (30)
else                     → any            (0)
```

#### Example:
```
hit_chance = 0.199 (19.9%)
→ Enum: any (0)
```

---

### 8. Confidence Score Adjustments
*Location: `HybridPrediction.cpp:1443, EdgeCaseDetection.h`*

Modifies final hitchance based on situational factors:

```cpp
// Base confidence factors
confidence *= (1.0 - distance * 0.0005)    // Further = less confident
confidence *= (1.0 - latency_ms * 0.01)    // Lag = less confident

// Edge case bonuses
if (target.is_cc'd):
    confidence *= 1.5  // +50% (stunned/rooted)
if (target.is_channeling):
    confidence *= 1.4  // +40% (Velkoz R, etc.)
if (target.is_dashing):
    confidence *= 0.7  // -30% (unpredictable)
if (target.has_shield):
    confidence = 0.0   // Won't hit (spell shield)

// Edge case multipliers are applied AFTER fusion
result.hit_chance *= edge_cases.confidence_multiplier
```

---

## Casting Decision Logic

### User-Specified Threshold

When you create a spell:
```cpp
spell_data.expected_hitchance = pred_sdk::hitchance::high;  // 70%
```

You're saying: **"Only cast if hit chance ≥ 70%"**

### The Comparison

```cpp
auto result = prediction->predict(spell_data);

if (result.is_valid && result.hitchance >= spell_data.expected_hitchance)
{
    // CAST THE SPELL
    player->cast_spell(slot, result.cast_position);
}
else
{
    // HOLD (not confident enough)
}
```

**Important**: The comparison is **enum vs enum**, not float!

```cpp
// Internal check
if (result.hitchance >= spell_data.expected_hitchance)

// Translates to integer comparison
if (70 >= 70)  // high >= high → TRUE, cast!
if (50 >= 70)  // medium >= high → FALSE, hold
```

---

## Complete Example Walkthrough

### Scenario: Xerath Q on Ezreal

#### Inputs:
```cpp
// Spell config
spell.projectile_speed = 500 u/s
spell.delay = 0.5s
spell.radius = 100 units
spell.range = 1400 units
spell.expected_hitchance = pred_sdk::hitchance::high  // Want 70%+

// Game state
source_pos = (1000, 0, 1000)
target_pos = (1800, 0, 1000)  // 800 units away
target_velocity = (100, 0, 100)  // Moving forward-right
target_move_speed = 350 u/s
```

#### Step 1: Arrival Time
```
distance = 800 units
arrival_time = 0.5 + (800 / 500) = 2.1 seconds
```

#### Step 2: Reachable Region
```
current_speed = 141 u/s
max_radius = 716.8 units (calculated above)
reachable_area = 1,613,770 sq units
```

#### Step 3: Behavior PDF
```
Ezreal's patterns (last 45 samples):
- 65% forward (toward turret)
- 25% kiting backward
- 10% side-dodge

We aim at his forward path.
```

#### Step 4: Physics Probability
```
Our spell circle at predicted position:
intersection_area = 31,416 sq units

P_physics = 31,416 / 1,613,770 = 0.0195 (1.95%)
```

#### Step 5: Behavior Probability
```
Spell overlaps with "forward" cluster:
P_behavior = 0.65 (65%)
```

#### Step 6: Fusion
```
sample_count = 45 (abundant)
physics_weight = 0.3
behavior_weight = 0.7
time_since_update = 0.08s (fresh)

hit_chance = (0.0195^0.3) × (0.65^0.7) × 0.85
           = 0.305 × 0.713 × 0.85
           = 0.185 (18.5%)
```

#### Step 7: Enum Conversion
```
hit_chance = 0.185 → pred_sdk::hitchance::any (0)
```

#### Step 8: Cast Decision
```cpp
if (result.hitchance >= spell_data.expected_hitchance)
// if (any >= high)
// if (0 >= 70)
// FALSE → DON'T CAST

// Reason: Physics probability too low (target can dodge easily)
```

---

## Why The System Works

### 1. **Physics Guards Against Impossible Shots**
Even if behavior says 100% forward movement, if the target is too far and can dodge easily, physics probability is low → final hitchance is low.

### 2. **Behavior Captures Predictability**
A target running in a straight line has high behavior probability even if physics says they *could* dodge.

### 3. **Adaptive Fusion Handles Data Quality**
- **Early game** (few samples): Trust physics more
- **Late game** (many samples): Trust learned patterns more
- **Fog/lag** (stale data): Automatically shift to physics

### 4. **Enum Abstraction Simplifies Usage**
Users think in terms like "high hitchance" instead of "67.3% probability".

---

## Tuning Recommendations

### For Aggressive Casting (More casts, lower accuracy):
```cpp
spell.expected_hitchance = pred_sdk::hitchance::medium;  // 50%
```

### For Conservative Casting (Fewer casts, higher accuracy):
```cpp
spell.expected_hitchance = pred_sdk::hitchance::very_high;  // 85%
```

### For Poke/Harass Spells:
```cpp
spell.expected_hitchance = pred_sdk::hitchance::high;  // 70%
// Good balance: frequent enough, accurate enough
```

### For All-In Combos:
```cpp
spell.expected_hitchance = pred_sdk::hitchance::very_high;  // 85%
// Only cast when very confident (combos require hitting)
```

---

## Advanced: Opportunistic Casting

*Location: `HybridPrediction.cpp:820-889`*

The system also tracks **opportunity windows** to detect peak moments:

```cpp
// Tracks recent hit chances
window.peak_hit_chance = 0.75  // Best we've seen recently
current_hit_chance = 0.72

// Opportunity score = current / peak
opportunity_score = 0.72 / 0.75 = 0.96 (96% of peak)

// Adaptive threshold (decays over time)
adaptive_threshold = 0.70 - (elapsed_time * 0.05)  // Becomes less picky

if (current_hit_chance > adaptive_threshold && is_declining):
    is_peak_opportunity = true  // CAST NOW! Won't get better
```

**Use case**: Prevents "waiting forever" for perfect shot. After a few seconds, cast at "good enough" opportunity.

---

## Key Takeaways

1. **Hitchance is a fusion of physics (can they dodge?) and behavior (will they dodge?)**
2. **Casting decision is a simple threshold check: `result.hitchance >= expected_hitchance`**
3. **System automatically adapts to data quality (sample count, staleness)**
4. **Angular optimization improves linear spells by testing ±10° around predicted center**
5. **Lower expected_hitchance = more aggressive casting, higher = more conservative**

**The math is complex, but the API is simple**: Set your threshold, check the result!
