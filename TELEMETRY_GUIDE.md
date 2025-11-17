# Danny Prediction SDK - Telemetry Guide

## 🎯 What is Telemetry?

The telemetry system logs **every prediction** made during your game to a timestamped file. This data helps analyze:
- **Prediction accuracy** (how often high/low hitchance predictions occur)
- **Performance** (computation time per prediction)
- **Edge cases** (dash predictions, stasis timing, animation locks, etc.)
- **Pattern detection** (are behavior patterns being detected correctly?)
- **Per-champion stats** (which enemies are hardest to predict?)

---

## ✅ How to Enable Telemetry

### Option 1: Menu (Recommended)
1. In-game, open main menu
2. Navigate to **"Danny Prediction"**
3. Check **"Enable Telemetry"** ✅ (enabled by default)

### Option 2: Code (for developers)
```cpp
PredictionSettings::get().enable_telemetry = true;
```

---

## 📁 Where are the Log Files?

**File location**: Same directory as your game executable

**File name format**: `dannypred_telemetry_YYYYMMDD_HHMMSS.txt`

**Example**:
```
dannypred_telemetry_20250117_153045.txt
(Game played on 2025-01-17 at 15:30:45)
```

---

## 📊 What Data is Logged?

### Session Summary
- **Champion played**
- **Game duration**
- **Total predictions** (valid + invalid)
- **Performance metrics** (avg/min/max computation time)

### Hitchance Distribution
```
0-20%: 45 (5%)    ← Low confidence predictions
20-40%: 120 (14%)
40-60%: 280 (33%)
60-80%: 310 (36%)
80-100%: 95 (11%) ← High confidence predictions
```

### Edge Case Stats
```
Dash Predictions: 23       ← How often dash endpoints were predicted
Stasis Predictions: 4      ← Zhonya's/GA timing predictions
Channel Predictions: 7     ← Recall/channel interrupts
Stationary Targets: 156    ← Enemies standing still
Animation Locked: 89       ← AA windup predictions
Collision Detected: 67     ← Skillshots blocked by minions
```

### Per-Spell-Type Breakdown
```
circular: 420 predictions, avg hitchance 68%
linear: 280 predictions, avg hitchance 72%
targeted: 45 predictions, avg hitchance 100%
```

### Per-Target Breakdown
```
Ezreal: 180 predictions, avg hitchance 65%  ← High mobility = lower hitchance
Darius: 120 predictions, avg hitchance 78%  ← Low mobility = higher hitchance
```

### Detailed Event Log
```
[123.45s] Ezreal | circular | HC:65% | Conf:82% | Dist:850 | normal | 0.234ms
[124.12s] Darius | linear | HC:78% | Conf:91% | Dist:600 | normal [STILL] | 0.189ms
[125.67s] Yasuo | circular | HC:45% | Conf:68% | Dist:920 | dash [DASH] | 0.312ms
```

---

## 📤 How to Send Data for Analysis

### Step 1: Play a Game
- Telemetry is **automatically enabled** by default
- Play normally (no special setup needed)
- Telemetry logs in the background (zero performance impact)

### Step 2: End Game
- When you exit the game, telemetry auto-saves
- Console message: `"[Danny.Prediction] Telemetry finalized - check log file"`
- File is written to same directory as game executable

### Step 3: Locate the File
- Look for: `dannypred_telemetry_YYYYMMDD_HHMMSS.txt`
- Sort by **Date Modified** (most recent = your last game)

### Step 4: Send to Danny/Claude
- **Upload the entire .txt file** (don't edit or truncate)
- Include context:
  - "I felt predictions were too aggressive" → Check if hitchance distribution is skewed high
  - "Ezreal was impossible to hit" → Check Ezreal's avg hitchance + dodge patterns
  - "Game lagged during teamfights" → Check max computation time spikes
  - "Dash predictions were wrong" → Check dash prediction count + reasoning

---

## 🔍 What I'll Analyze

### 1. **Prediction Quality**
- Is hitchance distribution reasonable? (should be bell curve centered around 50-70%)
- Are edge cases handled correctly? (stasis, dash, animation lock)
- Are certain champions consistently hard to predict?

### 2. **Performance**
- Is computation time acceptable? (should be <1ms per prediction)
- Are there spikes during teamfights?
- Is grid search resolution appropriate for your hardware?

### 3. **Pattern Detection**
- Are dodge patterns being detected?
- Are alternating/repeating patterns flagged correctly?
- Is reaction delay weighting working?

### 4. **Edge Case Detection**
- Are dash predictions accurate?
- Is stasis timing correct?
- Are animation locks being detected?

### 5. **Collision Detection**
- Are minion collisions being caught?
- Are ally vs enemy collisions handled correctly?

---

## 🎯 Example Use Cases

### Use Case 1: "My predictions feel too aggressive"
**What to check**:
```
--- HITCHANCE DISTRIBUTION ---
80-100%: 420 (50%)  ← TOO HIGH! Should be ~10-15%
60-80%: 280 (33%)
40-60%: 120 (14%)
20-40%: 20 (2%)
0-20%: 10 (1%)
```
**Diagnosis**: Confidence multiplier is too high or edge case boosts are over-tuned

### Use Case 2: "Ezreal is impossible to predict"
**What to check**:
```
--- PER TARGET ---
Ezreal: 180 predictions, avg hitchance 45%  ← Low! Expected for high mobility
Darius: 120 predictions, avg hitchance 78%  ← Normal!
```
**Diagnosis**: Ezreal's dodge patterns might not be learned correctly, or his dash predictions need tuning

### Use Case 3: "Game lags in teamfights"
**What to check**:
```
--- PERFORMANCE ---
Avg Computation Time: 0.234 ms
Min: 0.123 ms | Max: 3.456 ms  ← SPIKE!
```
**Diagnosis**: Grid search resolution might be too high (change from "Maximum" to "Performance" in menu)

### Use Case 4: "Dash predictions are always wrong"
**What to check**:
```
[125.67s] Yasuo | circular | HC:85% | Conf:95% | Dist:650 | dash [DASH] | 0.234ms
```
**Check reasoning**: Does it predict at dash endpoint when Yasuo cancels mid-dash? Need dash cancellation detection.

---

## 🛠️ Troubleshooting

### Q: Where is the log file?
**A**: Same directory as your game executable. Sort by "Date Modified" to find the most recent.

### Q: Telemetry is disabled, how do I enable it?
**A**: In-game menu → "Danny Prediction" → Check "Enable Telemetry"

### Q: Will telemetry slow down my game?
**A**: No. Telemetry has **zero overhead** when disabled, and **minimal overhead** when enabled (<0.001ms per prediction)

### Q: How big are the log files?
**A**: ~50-200 KB per game (30 minutes). Very small - won't fill your disk.

### Q: Can I delete old telemetry files?
**A**: Yes! Safe to delete. They're just text logs for analysis.

### Q: What if I want to disable telemetry?
**A**: In-game menu → "Danny Prediction" → Uncheck "Enable Telemetry"

---

## 📧 Sending Feedback

When sending telemetry files, include:
1. **The .txt file** (entire file, don't edit)
2. **Context**: What felt wrong? What were you playing? Who were you against?
3. **Your setup**: Performance mode? Quality mode? FPS during game?

Example message:
```
"Here's my telemetry from a game as Xerath vs Ezreal/Zed/Yasuo.

I felt like my Q predictions were always too far ahead - Ezreal would juke
sideways and my skillshot would miss where he was GOING to be, not where he
actually ended up.

Also, dash predictions on Yasuo felt wrong - he'd dash through minions and
my prediction would aim at his endpoint, but he'd cancel mid-dash.

Running on Performance mode, 60 FPS stable."
```

This helps me correlate your experience with the actual data!

---

## ✨ Future Telemetry Features (Planned)

- **Actual hit tracking** (requires spell tracking to know if prediction was correct)
- **Heatmap generation** (visualize where predictions aimed vs where targets actually went)
- **Real-time dashboard** (live stats during game)
- **Comparison mode** (compare your telemetry vs average player)

---

**Questions?** Send your telemetry file and I'll analyze it! 🚀
