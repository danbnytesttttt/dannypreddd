// CRITICAL: Must be FIRST - disables iterator debugging in ALL builds (Debug AND Release)
// This prevents linker error LNK2019: unresolved external symbol _CrtDbgReport
// We MUST disable this even in Debug builds to avoid CRT library conflicts
#undef _ITERATOR_DEBUG_LEVEL
#define _ITERATOR_DEBUG_LEVEL 0

#include "HybridPrediction.h"
#include "EdgeCaseDetection.h"
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <sstream>

// Reasoning string generation (expensive - disable for production)
// Set to 0 to disable reasoning strings (saves ~0.02ms per prediction)
#define HYBRID_PRED_ENABLE_REASONING 1

namespace HybridPred
{
    // =========================================================================
    // BEHAVIOR PDF IMPLEMENTATION
    // =========================================================================

    float BehaviorPDF::sample(const math::vector3& world_pos) const
    {
        // Convert world position to grid coordinates
        float dx = world_pos.x - origin.x;
        float dz = world_pos.z - origin.z;

        int grid_x = static_cast<int>((dx / cell_size) + GRID_SIZE / 2);
        int grid_z = static_cast<int>((dz / cell_size) + GRID_SIZE / 2);

        // Check bounds
        if (grid_x < 0 || grid_x >= GRID_SIZE || grid_z < 0 || grid_z >= GRID_SIZE)
            return 0.f;

        // Bilinear interpolation for smoother sampling
        float fx = (dx / cell_size) + GRID_SIZE / 2 - grid_x;
        float fz = (dz / cell_size) + GRID_SIZE / 2 - grid_z;

        float v00 = pdf_grid[grid_x][grid_z];
        float v10 = (grid_x + 1 < GRID_SIZE) ? pdf_grid[grid_x + 1][grid_z] : 0.f;
        float v01 = (grid_z + 1 < GRID_SIZE) ? pdf_grid[grid_x][grid_z + 1] : 0.f;
        float v11 = (grid_x + 1 < GRID_SIZE && grid_z + 1 < GRID_SIZE) ?
            pdf_grid[grid_x + 1][grid_z + 1] : 0.f;

        float v0 = v00 * (1.f - fx) + v10 * fx;
        float v1 = v01 * (1.f - fx) + v11 * fx;

        return v0 * (1.f - fz) + v1 * fz;
    }

    void BehaviorPDF::normalize()
    {
        total_probability = 0.f;

        // Sum all probabilities
        for (int i = 0; i < GRID_SIZE; ++i)
        {
            for (int j = 0; j < GRID_SIZE; ++j)
            {
                total_probability += pdf_grid[i][j];
            }
        }

        // Normalize so sum = 1
        if (total_probability > EPSILON)
        {
            float scale = 1.f / total_probability;
            for (int i = 0; i < GRID_SIZE; ++i)
            {
                for (int j = 0; j < GRID_SIZE; ++j)
                {
                    pdf_grid[i][j] *= scale;
                }
            }
            total_probability = 1.f;
        }
    }

    void BehaviorPDF::add_weighted_sample(const math::vector3& pos, float weight)
    {
        // Convert world position to grid coordinates
        float dx = pos.x - origin.x;
        float dz = pos.z - origin.z;

        int grid_x = static_cast<int>((dx / cell_size) + GRID_SIZE / 2);
        int grid_z = static_cast<int>((dz / cell_size) + GRID_SIZE / 2);

        // Gaussian kernel (spread probability to nearby cells)
        constexpr float sigma = 1.5f;
        constexpr int kernel_radius = 2;

        for (int i = -kernel_radius; i <= kernel_radius; ++i)
        {
            for (int j = -kernel_radius; j <= kernel_radius; ++j)
            {
                int gx = grid_x + i;
                int gz = grid_z + j;

                if (gx >= 0 && gx < GRID_SIZE && gz >= 0 && gz < GRID_SIZE)
                {
                    float dist_sq = static_cast<float>(i * i + j * j);
                    float kernel_value = std::exp(-dist_sq / (2.f * sigma * sigma));
                    pdf_grid[gx][gz] += weight * kernel_value;
                }
            }
        }
    }

    // =========================================================================
    // TARGET BEHAVIOR TRACKER IMPLEMENTATION
    // =========================================================================

    TargetBehaviorTracker::TargetBehaviorTracker(game_object* target)
        : target_(target), last_update_time_(0.f), last_aa_time_(0.f),
        cached_prediction_time_(-1.f), cached_move_speed_(-1.f), cached_timestamp_(-1.f),
        is_currently_stationary_(false), stationary_start_time_(0.f)
    {
    }

    void TargetBehaviorTracker::update()
    {
        if (!target_ || !target_->is_valid())
            return;

        float current_time = g_sdk->clock_facade->get_game_time();

        // Sample at fixed rate
        if (current_time - last_update_time_ < MOVEMENT_SAMPLE_RATE)
            return;

        // Create snapshot
        MovementSnapshot snapshot;
        snapshot.position = target_->get_position();
        snapshot.timestamp = current_time;

        snapshot.is_auto_attacking = is_auto_attacking(target_);
        snapshot.is_casting = is_casting_spell(target_);
        snapshot.is_dashing = target_->is_dashing();
        snapshot.is_cced = target_->has_buff_of_type(buff_type::stun) || target_->has_buff_of_type(buff_type::charm) ||
            target_->has_buff_of_type(buff_type::fear) || target_->has_buff_of_type(buff_type::snare) || target_->has_buff_of_type(buff_type::taunt) ||
            target_->has_buff_of_type(buff_type::suppression) || target_->has_buff_of_type(buff_type::knockup);

        // Safety: Prevent division by zero
        float max_hp = target_->get_max_hp();
        snapshot.hp_percent = (max_hp > 0.f) ? (target_->get_hp() / max_hp) * 100.f : 100.f;

        // Compute velocity if we have previous snapshot
        if (!movement_history_.empty())
        {
            snapshot.velocity = compute_velocity(movement_history_.back(), snapshot);

            // Detect auto-attack for post-AA movement analysis
            if (snapshot.is_auto_attacking && !movement_history_.back().is_auto_attacking)
            {
                last_aa_time_ = current_time;
            }

            // Track post-AA movement delay
            if (last_aa_time_ > 0.f && snapshot.velocity.magnitude() > 10.f &&
                movement_history_.back().velocity.magnitude() < 10.f)
            {
                float delay = current_time - last_aa_time_;
                if (delay < 1.0f) // Only track reasonable delays
                {
                    post_aa_movement_delays_.push_back(delay);
                    if (post_aa_movement_delays_.size() > 20)
                        post_aa_movement_delays_.erase(post_aa_movement_delays_.begin());
                }
            }

            // Stationary target detection (NEW)
            float velocity_magnitude = snapshot.velocity.magnitude();
            bool was_stationary = is_currently_stationary_;

            if (velocity_magnitude < STATIONARY_VELOCITY_THRESHOLD)
            {
                // Target is stationary
                if (!was_stationary)
                {
                    // Just became stationary - start timer
                    is_currently_stationary_ = true;
                    stationary_start_time_ = current_time;
                }
                // else: still stationary, timer keeps running
            }
            else
            {
                // Target is moving - reset stationary state
                if (was_stationary)
                {
                    is_currently_stationary_ = false;
                    stationary_start_time_ = 0.f;
                }
            }
        }

        // Add to history
        movement_history_.push_back(snapshot);

        // Limit history size
        if (movement_history_.size() > MOVEMENT_HISTORY_SIZE)
            movement_history_.pop_front();

        last_update_time_ = current_time;

        // Analyze patterns periodically
        if (movement_history_.size() >= MIN_SAMPLES_FOR_BEHAVIOR &&
            static_cast<int>(movement_history_.size()) % 20 == 0)
        {
            analyze_patterns();
        }
    }

    void TargetBehaviorTracker::analyze_patterns()
    {
        update_dodge_pattern();
        detect_direction_changes();
    }

    math::vector3 TargetBehaviorTracker::compute_velocity(
        const MovementSnapshot& prev,
        const MovementSnapshot& curr) const
    {
        float dt = curr.timestamp - prev.timestamp;
        if (dt < EPSILON)
            return math::vector3{};

        return (curr.position - prev.position) / dt;
    }

    void TargetBehaviorTracker::update_dodge_pattern()
    {
        if (movement_history_.size() < 3)
            return;

        int left_count = 0, right_count = 0, forward_count = 0, backward_count = 0;
        int total_movements = 0;

        // Analyze movement directions relative to previous direction
        for (size_t i = 2; i < movement_history_.size(); ++i)
        {
            const auto& prev = movement_history_[i - 1];
            const auto& curr = movement_history_[i];

            if (prev.velocity.magnitude() < 10.f || curr.velocity.magnitude() < 10.f)
                continue;

            // Compute perpendicular and parallel components
            math::vector3 prev_dir = prev.velocity.normalized();
            math::vector3 curr_dir = curr.velocity.normalized();

            // Cross product to determine left/right (y component)
            float cross_y = prev_dir.x * curr_dir.z - prev_dir.z * curr_dir.x;

            // Dot product to determine forward/backward
            float dot = prev_dir.dot(curr_dir);

            if (cross_y > 0.1f) left_count++;
            else if (cross_y < -0.1f) right_count++;

            if (dot > 0.5f) forward_count++;
            else if (dot < -0.5f) backward_count++;

            total_movements++;
        }

        if (total_movements > 0)
        {
            float inv_total = 1.f / total_movements;
            dodge_pattern_.left_dodge_frequency = left_count * inv_total;
            dodge_pattern_.right_dodge_frequency = right_count * inv_total;
            dodge_pattern_.forward_frequency = forward_count * inv_total;
            dodge_pattern_.backward_frequency = backward_count * inv_total;

            // Linear continuation probability
            dodge_pattern_.linear_continuation_prob = forward_count * inv_total;
        }

        // Update reaction delay from post-AA movement data
        dodge_pattern_.reaction_delay = 200.f; // Default 200ms

        if (!post_aa_movement_delays_.empty())
        {
            float sum = 0.f;
            for (float delay : post_aa_movement_delays_)
                sum += delay;
            dodge_pattern_.reaction_delay = (sum / post_aa_movement_delays_.size()) * 1000.f;
        }

        // PATTERN REPETITION DETECTION
        // PATTERN EXPIRATION: Reset pattern if no movement updates for 3+ seconds
        if (!movement_history_.empty())
        {
            float current_time = g_sdk->clock_facade->get_game_time();
            float last_movement_time = movement_history_.back().timestamp;
            constexpr float PATTERN_EXPIRY_DURATION = 3.0f;  // 3 seconds of inactivity

            if (dodge_pattern_.has_pattern &&
                (current_time - last_movement_time > PATTERN_EXPIRY_DURATION))
            {
                // Pattern is stale - reset it
                dodge_pattern_.has_pattern = false;
                dodge_pattern_.pattern_confidence = 0.f;
                dodge_pattern_.predicted_next_direction = math::vector3{};
                dodge_pattern_.juke_sequence.clear();
            }
        }

        // Build juke sequence from recent direction changes
        dodge_pattern_.juke_sequence.clear();
        constexpr size_t MAX_SEQUENCE_LENGTH = 8;

        for (size_t i = movement_history_.size() > MAX_SEQUENCE_LENGTH + 1 ?
            movement_history_.size() - MAX_SEQUENCE_LENGTH : 1;
            i < movement_history_.size(); ++i)
        {
            const auto& prev = movement_history_[i - 1];
            const auto& curr = movement_history_[i];

            if (prev.velocity.magnitude() < 10.f || curr.velocity.magnitude() < 10.f)
                continue;

            math::vector3 prev_dir = prev.velocity.normalized();
            math::vector3 curr_dir = curr.velocity.normalized();

            // Cross product Y component determines left (-1) or right (1)
            float cross_y = prev_dir.x * curr_dir.z - prev_dir.z * curr_dir.x;

            if (cross_y > 0.15f)
                dodge_pattern_.juke_sequence.push_back(-1);  // Left
            else if (cross_y < -0.15f)
                dodge_pattern_.juke_sequence.push_back(1);   // Right
            else
                dodge_pattern_.juke_sequence.push_back(0);   // Straight
        }

        // Detect alternating pattern (L-R-L-R or R-L-R-L)
        if (dodge_pattern_.juke_sequence.size() >= 4)
        {
            bool is_alternating = true;
            int alternation_count = 0;

            for (size_t i = 2; i < dodge_pattern_.juke_sequence.size(); ++i)
            {
                int prev_juke = dodge_pattern_.juke_sequence[i - 2];
                int curr_juke = dodge_pattern_.juke_sequence[i];

                // Skip straights
                if (prev_juke == 0 || curr_juke == 0)
                    continue;

                // Check if opposite direction
                if (prev_juke == -curr_juke)
                    alternation_count++;
                else
                    is_alternating = false;
            }

            if (is_alternating && alternation_count >= 2)
            {
                // Alternating pattern detected!
                dodge_pattern_.has_pattern = true;
                dodge_pattern_.pattern_confidence = std::min(0.9f, 0.6f + alternation_count * 0.1f);
                // Pattern update time tracking disabled for standalone

                // Predict next juke: opposite of last
                int last_juke = dodge_pattern_.juke_sequence.back();
                if (last_juke != 0 && !movement_history_.empty())
                {
                    const auto& latest = movement_history_.back();
                    math::vector3 vel_dir = latest.velocity.normalized();
                    // Perpendicular: 90° rotation in XZ plane
                    math::vector3 perpendicular(-vel_dir.z, 0.f, vel_dir.x);
                    dodge_pattern_.predicted_next_direction = perpendicular * static_cast<float>(-last_juke);
                }
            }
            // Detect repeating sequence (e.g., L-L-R-L-L-R)
            else if (dodge_pattern_.juke_sequence.size() >= 6)
            {
                // Check if first half matches second half
                size_t half = dodge_pattern_.juke_sequence.size() / 2;
                bool is_repeating = true;

                for (size_t i = 0; i < half; ++i)
                {
                    if (dodge_pattern_.juke_sequence[i] != dodge_pattern_.juke_sequence[i + half])
                    {
                        is_repeating = false;
                        break;
                    }
                }

                if (is_repeating)
                {
                    // Repeating sequence detected!
                    dodge_pattern_.has_pattern = true;
                    dodge_pattern_.pattern_confidence = 0.85f;
                    if (g_sdk && g_sdk->clock_facade)
                        dodge_pattern_.last_pattern_update_time = g_sdk->clock_facade->get_game_time();

                    // Predict next: continues the sequence
                    int next_in_sequence = dodge_pattern_.juke_sequence[dodge_pattern_.juke_sequence.size() % half];
                    if (next_in_sequence != 0 && !movement_history_.empty())
                    {
                        const auto& latest = movement_history_.back();
                        math::vector3 vel_dir = latest.velocity.normalized();
                        math::vector3 perpendicular(-vel_dir.z, 0.f, vel_dir.x);
                        dodge_pattern_.predicted_next_direction = perpendicular * static_cast<float>(next_in_sequence);
                    }
                }
            }
        }

        // No pattern detected
        if (!dodge_pattern_.has_pattern)
        {
            dodge_pattern_.pattern_confidence = 0.f;
            dodge_pattern_.predicted_next_direction = math::vector3{};
        }
    }

    void TargetBehaviorTracker::detect_direction_changes()
    {
        direction_change_times_.clear();
        direction_change_angles_.clear();

        if (movement_history_.size() < 3)
            return;

        for (size_t i = 2; i < movement_history_.size(); ++i)
        {
            const auto& prev = movement_history_[i - 2];
            const auto& mid = movement_history_[i - 1];
            const auto& curr = movement_history_[i];

            if (prev.velocity.magnitude() < 10.f || curr.velocity.magnitude() < 10.f)
                continue;

            // Detect significant direction change
            math::vector3 prev_dir = prev.velocity.normalized();
            math::vector3 curr_dir = curr.velocity.normalized();

            float angle = std::acos(std::clamp(prev_dir.dot(curr_dir), -1.f, 1.f));

            if (angle > 0.5f) // ~30 degrees
            {
                direction_change_times_.push_back(curr.timestamp);
                direction_change_angles_.push_back(angle);
            }
        }

        // Compute juke interval statistics
        if (direction_change_times_.size() >= 2)
        {
            std::vector<float> intervals;
            for (size_t i = 1; i < direction_change_times_.size(); ++i)
            {
                intervals.push_back(direction_change_times_[i] - direction_change_times_[i - 1]);
            }

            // Mean
            float sum = 0.f;
            for (float interval : intervals)
                sum += interval;
            dodge_pattern_.juke_interval_mean = sum / intervals.size();

            // Variance
            float variance_sum = 0.f;
            for (float interval : intervals)
            {
                float diff = interval - dodge_pattern_.juke_interval_mean;
                variance_sum += diff * diff;
            }
            dodge_pattern_.juke_interval_variance = variance_sum / intervals.size();
        }
    }

    bool TargetBehaviorTracker::is_animation_locked() const
    {
        if (movement_history_.empty())
            return false;

        const auto& latest = movement_history_.back();
        return latest.is_auto_attacking || latest.is_casting || latest.is_cced;
    }

    math::vector3 TargetBehaviorTracker::get_current_velocity() const
    {
        if (movement_history_.empty())
            return math::vector3{};

        return movement_history_.back().velocity;
    }

    float TargetBehaviorTracker::get_stationary_duration(float current_time) const
    {
        if (!is_currently_stationary_)
            return 0.f;

        return current_time - stationary_start_time_;
    }

    float TargetBehaviorTracker::get_stationary_hitchance_boost(float current_time) const
    {
        if (!is_currently_stationary_)
            return 0.f;  // No boost - target is moving

        float duration = get_stationary_duration(current_time);

        // Scaling: 0.5s = 50%, 1.0s+ = 75%
        if (duration < 0.5f)
            return 0.f;  // Less than 0.5s - no boost

        if (duration >= 1.0f)
            return 0.75f;  // 1.0s or more - 75% minimum hitchance

        // Linear interpolation between 0.5s and 1.0s
        // duration = 0.5s → 50%
        // duration = 1.0s → 75%
        float t = (duration - 0.5f) / 0.5f;  // Normalized to [0,1]
        return 0.50f + t * 0.25f;  // Interpolate from 50% to 75%
    }

    BehaviorPDF TargetBehaviorTracker::build_behavior_pdf(float prediction_time, float move_speed) const
    {
        // PDF caching: Reuse cached PDF if same frame and similar parameters
        // This avoids rebuilding for Q/W/E/R predictions on the same target in one frame
        float current_time = g_sdk->clock_facade->get_game_time();
        constexpr float TIME_TOLERANCE = 0.05f;  // 50ms tolerance for prediction_time similarity
        constexpr float SPEED_TOLERANCE = 20.f;  // 20 units/s tolerance for move_speed

        bool same_frame = std::abs(current_time - cached_timestamp_) < EPSILON;
        bool similar_pred_time = std::abs(prediction_time - cached_prediction_time_) < TIME_TOLERANCE;
        bool similar_move_speed = std::abs(move_speed - cached_move_speed_) < SPEED_TOLERANCE;

        if (same_frame && similar_pred_time && similar_move_speed && cached_pdf_.total_probability > EPSILON)
        {
            // Cache hit - return cached PDF
            return cached_pdf_;
        }

        BehaviorPDF pdf;

        if (movement_history_.empty())
            return pdf;

        const auto& latest = movement_history_.back();

        // DYNAMIC CELL SIZE: Ensure grid covers maximum distance target can move
        // Grid radius = (GRID_SIZE / 2) * cell_size
        // Target max movement = move_speed * prediction_time
        // Add 20% margin for dodge patterns
        // Use move_speed stat instead of velocity.magnitude() (which can be 0 if target is mid-cast)
        float max_move_distance = move_speed * prediction_time * 1.2f;
        float required_grid_radius = std::max(400.f, max_move_distance);  // Minimum 400 units
        pdf.cell_size = (required_grid_radius * 2.f) / BehaviorPDF::GRID_SIZE;  // Total coverage / grid_size

        // ADAPTIVE DECAY RATE: Adjust based on target mobility
        float decay_rate = get_adaptive_decay_rate(latest.velocity.magnitude());

        // If animation locked, predict stationary at current position
        if (latest.is_cced || latest.is_casting)
        {
            pdf.origin = latest.position;
            pdf.add_weighted_sample(latest.position, 1.0f);
            pdf.normalize();
            return pdf;
        }

        // First pass: Compute weighted average of predicted positions
        // This centers the grid where samples will actually fall, not just where latest velocity predicts
        math::vector3 predicted_center{};
        float total_weight = 0.f;
        int sample_count = 0;

        for (size_t i = 0; i < movement_history_.size() && sample_count < 30; ++i)
        {
            size_t idx = movement_history_.size() - 1 - i;
            const auto& snapshot = movement_history_[idx];

            // Exponential decay weighting (recent data more important)
            // Use adaptive decay rate based on target mobility
            float weight = std::pow(decay_rate, static_cast<float>(i));

            // Predict position from this snapshot
            math::vector3 predicted_pos = snapshot.position + snapshot.velocity * prediction_time;

            // Accumulate for weighted average
            predicted_center = predicted_center + predicted_pos * weight;
            total_weight += weight;
            sample_count++;
        }

        // Center grid at weighted average of all predictions
        // This handles direction changes gracefully - grid is centered where samples cluster
        if (total_weight > EPSILON)
        {
            predicted_center = predicted_center / total_weight;
        }
        else
        {
            // Fallback to simple linear prediction
            predicted_center = latest.position + latest.velocity * prediction_time;
        }
        pdf.origin = predicted_center;

        // Second pass: Add samples to PDF (now properly centered)
        total_weight = 0.f;
        sample_count = 0;

        for (size_t i = 0; i < movement_history_.size() && sample_count < 30; ++i)
        {
            size_t idx = movement_history_.size() - 1 - i;
            const auto& snapshot = movement_history_[idx];

            // Exponential decay weighting (recent data more important)
            // Use adaptive decay rate based on target mobility
            float weight = std::pow(decay_rate, static_cast<float>(i));

            // Predict position from this snapshot
            math::vector3 predicted_pos = snapshot.position + snapshot.velocity * prediction_time;

            // Add to PDF with weight
            pdf.add_weighted_sample(predicted_pos, weight);
            total_weight += weight;
            sample_count++;
        }

        // Apply dodge pattern bias ONLY if target has had time to react
        // Reaction delay gating: Skip lateral dodge samples if prediction_time < reaction_delay
        // Target cannot have juked yet if they haven't had time to react
        float reaction_delay_seconds = dodge_pattern_.reaction_delay / 1000.f;  // Convert ms to seconds
        bool can_react = prediction_time >= reaction_delay_seconds;

        if (latest.velocity.magnitude() > 10.f && can_react)
        {
            math::vector3 velocity_dir = latest.velocity.normalized();
            // Perpendicular in XZ plane (2D movement) - 90° rotation
            math::vector3 perpendicular(-velocity_dir.z, 0.f, velocity_dir.x);

            // Compute forward and lateral displacement separately
            math::vector3 forward = latest.velocity * prediction_time;

            // LEARNED lateral dodge factor from actual movement patterns
            // Compute average lateral component from direction changes
            float lateral_factor = 0.5f;  // Default fallback
            if (direction_change_angles_.size() >= 3)
            {
                // Average absolute lateral component (sin of angle)
                float total_lateral = 0.f;
                for (float angle : direction_change_angles_)
                {
                    total_lateral += std::abs(std::sin(angle));
                }
                lateral_factor = total_lateral / direction_change_angles_.size();
                lateral_factor = std::clamp(lateral_factor, 0.2f, 0.9f);  // Reasonable bounds
            }
            float dodge_distance = latest.velocity.magnitude() * prediction_time * lateral_factor;
            math::vector3 side = perpendicular * dodge_distance;

            // Juke cadence weighting: Weight lateral dodge samples based on timing
            // If prediction_time is close to juke_interval_mean, the target is likely to juke
            // Use Gaussian weighting: higher weight near typical juke time, lower weight far from it
            float juke_cadence_weight = 1.0f;  // Default
            if (dodge_pattern_.juke_interval_variance > EPSILON)
            {
                float sigma = std::sqrt(dodge_pattern_.juke_interval_variance);
                float time_diff = prediction_time - dodge_pattern_.juke_interval_mean;
                // Gaussian: k = exp(-0.5 * ((t - μ) / σ)²)
                juke_cadence_weight = std::exp(-0.5f * (time_diff * time_diff) / (sigma * sigma));
                // Clamp to reasonable range [0.3, 1.0] - still apply some dodge bias even off-rhythm
                juke_cadence_weight = std::clamp(juke_cadence_weight, 0.3f, 1.0f);
            }

            if (dodge_pattern_.left_dodge_frequency > 0.3f)
            {
                math::vector3 left_pos = latest.position + forward + side;
                pdf.add_weighted_sample(left_pos, dodge_pattern_.left_dodge_frequency * 0.5f * juke_cadence_weight);
            }

            if (dodge_pattern_.right_dodge_frequency > 0.3f)
            {
                math::vector3 right_pos = latest.position + forward - side;
                pdf.add_weighted_sample(right_pos, dodge_pattern_.right_dodge_frequency * 0.5f * juke_cadence_weight);
            }

            // PATTERN-BASED PREDICTION BOOST
            // If we detected a repeating pattern, heavily weight the predicted next juke
            if (dodge_pattern_.has_pattern && dodge_pattern_.pattern_confidence > 0.6f)
            {
                // Predict position based on detected pattern
                float pattern_distance = latest.velocity.magnitude() * prediction_time;
                math::vector3 pattern_predicted_pos = latest.position +
                    latest.velocity * prediction_time +
                    dodge_pattern_.predicted_next_direction * (pattern_distance * lateral_factor);

                // Heavy weight: pattern confidence dictates how much we trust this
                // Use 2x-3x normal weight to make pattern predictions dominant
                float pattern_weight = dodge_pattern_.pattern_confidence * 2.5f;
                pdf.add_weighted_sample(pattern_predicted_pos, pattern_weight);
            }
        }

        pdf.normalize();

        // Update cache for next call
        cached_pdf_ = pdf;
        cached_prediction_time_ = prediction_time;
        cached_move_speed_ = move_speed;
        cached_timestamp_ = current_time;

        return pdf;
    }

    OpportunityWindow& TargetBehaviorTracker::get_opportunity_window(int spell_slot) const
    {
        // Create if doesn't exist (mutable map allows modification in const method)
        if (opportunity_windows_.find(spell_slot) == opportunity_windows_.end())
        {
            opportunity_windows_[spell_slot] = OpportunityWindow();
            // Safety: Only set window start time if SDK is valid
            if (g_sdk && g_sdk->clock_facade)
            {
                opportunity_windows_[spell_slot].window_start_time = g_sdk->clock_facade->get_game_time();
            }
        }
        return opportunity_windows_[spell_slot];
    }

    // =========================================================================
    // OPPORTUNITY WINDOW IMPLEMENTATION
    // =========================================================================

    void OpportunityWindow::update(float current_time, float hit_chance)
    {
        // Add current sample to history
        history.push_back({ current_time, hit_chance });

        // MEMORY SAFETY: Cap deque size to prevent unbounded growth during lag spikes
        // 240Hz × 10s lag = 2400 entries without cap - limit to 200
        constexpr size_t MAX_HISTORY_SIZE = 200;
        while (history.size() > MAX_HISTORY_SIZE)
        {
            history.pop_front();
        }

        // Remove samples older than 3 seconds
        constexpr float WINDOW_DURATION = 3.0f;
        while (!history.empty() && current_time - history.front().first > WINDOW_DURATION)
        {
            history.pop_front();
        }

        // Update peak if this is better
        if (hit_chance > peak_hit_chance)
        {
            peak_hit_chance = hit_chance;
            peak_timestamp = current_time;
        }

        // Reset peak if too old (more than 2 seconds ago)
        if (current_time - peak_timestamp > 2.0f)
        {
            // Recalculate peak from current window
            peak_hit_chance = 0.f;
            for (const auto& sample : history)
            {
                if (sample.second > peak_hit_chance)
                {
                    peak_hit_chance = sample.second;
                    peak_timestamp = sample.first;
                }
            }
        }
    }

    bool OpportunityWindow::is_peak_opportunity(float current_time, float hit_chance, float adaptive_threshold, float elapsed_time, float patience_window) const
    {
        // SAFEGUARD 1: Adaptive Patience Window
        // Don't flag peaks too early - wait patience_window seconds
        // Patience adapts to spell cooldown: short CD (3s) = 1.5s, long CD (90s+) = 3.0s
        // This prevents casting on first tiny peak when better opportunity might come
        if (elapsed_time < patience_window)
            return false;

        if (history.size() < 5)  // Need at least 5 samples to detect trend
            return false;

        // SAFEGUARD 2: Minimum Quality
        // Peak must be within 10% of adaptive_threshold to be worth taking
        // Prevents casting on 40% "peak" when threshold is 80%
        if (hit_chance < adaptive_threshold * 0.90f)
            return false;

        // Check if this is a local maximum
        // Compare current hit_chance to recent average (last 1 second)
        float recent_sum = 0.f;
        int recent_count = 0;
        for (auto it = history.rbegin(); it != history.rend() && current_time - it->first < 1.0f; ++it)
        {
            recent_sum += it->second;
            recent_count++;
        }

        if (recent_count < 3)
            return false;

        float recent_avg = recent_sum / recent_count;

        // Current hit_chance must be above recent average (we're at a peak)
        if (hit_chance < recent_avg * 1.05f)  // 5% margin
            return false;

        // SAFEGUARD 3: Sustained Decline
        // Check if declining for 3+ consecutive samples (not just 1-2)
        // This prevents casting on random noise/blips
        if (history.size() >= 4)
        {
            float sample_4_ago = history[history.size() - 4].second;
            float sample_3_ago = history[history.size() - 3].second;
            float sample_2_ago = history[history.size() - 2].second;
            float sample_1_ago = history[history.size() - 1].second;

            // SUSTAINED declining trend: 3+ consecutive drops
            bool is_sustained_decline = (sample_1_ago < sample_2_ago) &&
                (sample_2_ago < sample_3_ago) &&
                (sample_3_ago < sample_4_ago);

            if (is_sustained_decline)
                return true;  // Safe to cast - sustained decline confirmed!
        }

        return false;
    }

    float OpportunityWindow::get_adaptive_threshold(float base_threshold, float elapsed_time) const
    {
        // Adaptive threshold decay: lower standards over time
        // 0-3s: Full threshold (no decay)
        // 3-8s: Linear decay to 70% of threshold
        // 8s+: Minimum 70% of original threshold

        if (elapsed_time < 3.0f)
            return base_threshold;  // No decay yet

        if (elapsed_time < 8.0f)
        {
            // Linear interpolation: 100% → 70% over 5 seconds
            float decay_factor = 1.0f - ((elapsed_time - 3.0f) / 5.0f) * 0.3f;
            return base_threshold * decay_factor;
        }

        // Minimum: 70% of original
        return base_threshold * 0.7f;
    }

    void HybridFusionEngine::update_opportunity_signals(
        HybridPredictionResult& result,
        game_object* source,
        const pred_sdk::spell_data& spell,
        TargetBehaviorTracker& tracker)
    {
        // Safety: Validate SDK before accessing
        if (!g_sdk || !g_sdk->clock_facade)
        {
            // Fallback: No opportunistic signals
            result.is_peak_opportunity = false;
            result.opportunity_score = 0.f;
            result.adaptive_threshold = 0.65f;  // Default high threshold
            return;
        }

        float current_time = g_sdk->clock_facade->get_game_time();
        int spell_slot = spell.spell_slot;

        // ADAPTIVE PATIENCE: Calculate patience window based on spell cooldown
        float spell_cooldown = 10.0f;  // Default fallback
        if (source && source->is_valid() && spell_slot >= 0 && spell_slot <= 3)
        {
            spell_entry* spell_entry_ptr = source->get_spell(spell_slot);
            if (spell_entry_ptr)
            {
                spell_cooldown = spell_entry_ptr->get_cooldown();
            }
        }
        float patience_window = std::clamp(spell_cooldown * 0.3f, 1.5f, 3.0f);

        // Safety: Validate spell slot (should be 0-3 for Q/W/E/R, or special slots)
        if (spell_slot < -1 || spell_slot > 10)
        {
            // Invalid spell slot - use slot 0 as fallback
            spell_slot = 0;
        }

        // Get or create opportunity window for this spell
        OpportunityWindow& window = tracker.get_opportunity_window(spell_slot);

        // Update window with current hit_chance
        window.update(current_time, result.hit_chance);

        // Calculate elapsed time since window started
        float elapsed_time = current_time - window.window_start_time;

        // Calculate opportunity score: how good is this moment relative to recent peak?
        if (window.peak_hit_chance > EPSILON)
        {
            result.opportunity_score = result.hit_chance / window.peak_hit_chance;
        }
        else
        {
            result.opportunity_score = 1.0f;  // First sample
        }

        // Calculate adaptive threshold FIRST (needed for peak detection)
        // Convert hitchance enum to float threshold
        float base_threshold = 0.65f;  // Default high
        if (spell.expected_hitchance == pred_sdk::hitchance::very_high)
            base_threshold = 0.80f;
        else if (spell.expected_hitchance == pred_sdk::hitchance::high)
            base_threshold = 0.65f;
        else if (spell.expected_hitchance == pred_sdk::hitchance::medium)
            base_threshold = 0.55f;
        else if (spell.expected_hitchance == pred_sdk::hitchance::low)
            base_threshold = 0.45f;

        result.adaptive_threshold = window.get_adaptive_threshold(base_threshold, elapsed_time);

        // Detect if this is a peak opportunity (uses adaptive_threshold, elapsed_time, and patience_window)
        result.is_peak_opportunity = window.is_peak_opportunity(current_time, result.hit_chance,
            result.adaptive_threshold, elapsed_time,
            patience_window);

        // Reset window if spell was likely cast (hit_chance suddenly drops or becomes invalid)
        // This happens when target moves out of range or dies
        if (result.hit_chance < window.last_hit_chance * 0.5f && elapsed_time > 1.0f)
        {
            // Significant drop - likely cast occurred, reset window
            window = OpportunityWindow();
            window.window_start_time = current_time;
        }
        window.last_hit_chance = result.hit_chance;
    }

    // =========================================================================
    // PHYSICS PREDICTOR IMPLEMENTATION
    // =========================================================================

    ReachableRegion PhysicsPredictor::compute_reachable_region(
        const math::vector3& current_pos,
        const math::vector3& current_velocity,
        float prediction_time,
        float move_speed,
        float turn_rate,
        float acceleration)
    {
        ReachableRegion region;
        region.center = current_pos;

        if (prediction_time < EPSILON)
        {
            region.max_radius = 0.f;
            region.area = 0.f;
            return region;
        }

        // Maximum distance considering acceleration from current velocity
        float current_speed = current_velocity.magnitude();
        float speed_diff = move_speed - current_speed;

        float max_distance;
        if (speed_diff > 0.f && acceleration > 0.f)
        {
            // Time to reach max speed
            float accel_time = std::min(speed_diff / acceleration, prediction_time);

            // Distance during acceleration: d = v₀*t + 0.5*a*t²
            float accel_distance = current_speed * accel_time +
                0.5f * acceleration * accel_time * accel_time;

            // Distance at max speed
            float max_speed_time = prediction_time - accel_time;
            float max_speed_distance = move_speed * max_speed_time;

            max_distance = accel_distance + max_speed_distance;
        }
        else
        {
            // Already at or above max speed (or instant speed)
            max_distance = move_speed * prediction_time;
        }

        region.max_radius = max_distance;

        // NOTE: turn_rate parameter is currently unused because League of Legends champions
        // have instant turn rate (can change direction immediately). If implementing for games
        // with turn rate mechanics (e.g., Dota 2), reachable region should be a sector/cone
        // instead of a full circle.
        (void)turn_rate; // Suppress unused parameter warning

        // Discretize boundary (circle approximation - full 360° reachability)
        constexpr int BOUNDARY_POINTS = 32;
        for (int i = 0; i < BOUNDARY_POINTS; ++i)
        {
            float angle = (2.f * PI * i) / BOUNDARY_POINTS;
            math::vector3 boundary_point = current_pos;
            boundary_point.x += max_distance * std::cos(angle);
            boundary_point.z += max_distance * std::sin(angle);
            region.boundary_points.push_back(boundary_point);
        }

        // Area = πr²
        region.area = PI * max_distance * max_distance;

        return region;
    }

    math::vector3 PhysicsPredictor::predict_linear_position(
        const math::vector3& current_pos,
        const math::vector3& current_velocity,
        float prediction_time)
    {
        return current_pos + current_velocity * prediction_time;
    }

    float PhysicsPredictor::compute_physics_hit_probability(
        const math::vector3& cast_position,
        float projectile_radius,
        const ReachableRegion& reachable_region)
    {
        if (reachable_region.area < EPSILON)
            return 0.f;

        // Compute intersection area between projectile circle and reachable circle
        float intersection_area = circle_circle_intersection_area(
            cast_position, projectile_radius,
            reachable_region.center, reachable_region.max_radius
        );

        // Probability = intersection / reachable area
        return std::min(1.f, intersection_area / reachable_region.area);
    }

    float PhysicsPredictor::compute_arrival_time(
        const math::vector3& source_pos,
        const math::vector3& target_pos,
        float projectile_speed,
        float cast_delay)
    {
        float distance = (target_pos - source_pos).magnitude();

        if (projectile_speed < EPSILON || projectile_speed >= FLT_MAX / 2.f)
        {
            // Instant spell
            return cast_delay;
        }

        return cast_delay + (distance / projectile_speed);
    }

    float PhysicsPredictor::circle_circle_intersection_area(
        const math::vector3& c1, float r1,
        const math::vector3& c2, float r2)
    {
        // NUMERIC STABILITY: Guard against zero/negative radii
        constexpr float MIN_RADIUS = 1e-6f;
        if (r1 < MIN_RADIUS || r2 < MIN_RADIUS)
            return 0.f;

        float d = (c2 - c1).magnitude();

        // No intersection
        if (d >= r1 + r2)
            return 0.f;

        // Complete containment
        if (d <= std::abs(r1 - r2))
        {
            float smaller_r = std::min(r1, r2);
            return PI * smaller_r * smaller_r;
        }

        // NUMERIC STABILITY: Guard against division by zero when circles are at same position
        if (d < MIN_RADIUS)
        {
            float smaller_r = std::min(r1, r2);
            return PI * smaller_r * smaller_r;
        }

        // Partial intersection - use lens formula
        // A = r1²*arccos((d²+r1²-r2²)/(2*d*r1)) + r2²*arccos((d²+r2²-r1²)/(2*d*r2))
        //     - 0.5*sqrt((r1+r2-d)*(r1-r2+d)*(-r1+r2+d)*(r1+r2+d))

        float d2 = d * d;
        float r1_2 = r1 * r1;
        float r2_2 = r2 * r2;

        float alpha = std::acos(std::clamp((d2 + r1_2 - r2_2) / (2.f * d * r1), -1.f, 1.f));
        float beta = std::acos(std::clamp((d2 + r2_2 - r1_2) / (2.f * d * r2), -1.f, 1.f));

        float area = r1_2 * alpha + r2_2 * beta;

        // NUMERIC STABILITY: Guard against negative sqrt due to floating point errors
        float sqrt_term = (r1 + r2 - d) * (r1 - r2 + d) * (-r1 + r2 + d) * (r1 + r2 + d);
        if (sqrt_term > 0.f)
            area -= 0.5f * std::sqrt(sqrt_term);

        return area;
    }

    // =========================================================================
    // BEHAVIOR PREDICTOR IMPLEMENTATION
    // =========================================================================

    BehaviorPDF BehaviorPredictor::build_pdf_from_history(
        const TargetBehaviorTracker& tracker,
        float prediction_time,
        float move_speed)
    {
        BehaviorPDF pdf = tracker.build_behavior_pdf(prediction_time, move_speed);
        return pdf;
    }

    float BehaviorPredictor::compute_behavior_hit_probability(
        const math::vector3& cast_position,
        float projectile_radius,
        const BehaviorPDF& pdf)
    {
        // If PDF has no data (not normalized or empty), return neutral 1.0
        // This allows fallback to physics-only prediction when behavior data is insufficient
        if (pdf.total_probability < EPSILON)
            return 1.0f;

        // Direct grid summation (more accurate than Monte Carlo sampling)
        // Sum probability mass of all cells whose centers fall inside the hit circle
        float radius_sq = projectile_radius * projectile_radius;
        float prob = 0.f;

        for (int x = 0; x < BehaviorPDF::GRID_SIZE; ++x)
        {
            for (int z = 0; z < BehaviorPDF::GRID_SIZE; ++z)
            {
                // World position of cell center
                float wx = pdf.origin.x + (x - BehaviorPDF::GRID_SIZE / 2 + 0.5f) * pdf.cell_size;
                float wz = pdf.origin.z + (z - BehaviorPDF::GRID_SIZE / 2 + 0.5f) * pdf.cell_size;

                // Check if cell center is inside hit circle
                float dx = wx - cast_position.x;
                float dz = wz - cast_position.z;

                if (dx * dx + dz * dz <= radius_sq)
                {
                    prob += pdf.pdf_grid[x][z];
                }
            }
        }

        // PDF is normalized (sums to 1), so this sum is the exact hit probability
        return std::clamp(prob, 0.f, 1.f);
    }

    math::vector3 BehaviorPredictor::predict_from_behavior(
        const TargetBehaviorTracker& tracker,
        float prediction_time)
    {
        const auto& history = tracker.get_history();
        if (history.empty())
            return math::vector3{};

        const auto& latest = history.back();

        // Weighted average of predicted positions
        math::vector3 predicted_pos{};
        float total_weight = 0.f;

        // ADAPTIVE DECAY RATE: Adjust based on target mobility
        float decay_rate = get_adaptive_decay_rate(latest.velocity.magnitude());

        for (size_t i = 0; i < std::min(history.size(), size_t(20)); ++i)
        {
            size_t idx = history.size() - 1 - i;
            const auto& snapshot = history[idx];

            float weight = std::pow(decay_rate, static_cast<float>(i));
            predicted_pos = predicted_pos + (snapshot.position + snapshot.velocity * prediction_time) * weight;
            total_weight += weight;
        }

        if (total_weight > EPSILON)
            predicted_pos = predicted_pos / total_weight;

        return predicted_pos;
    }

    void BehaviorPredictor::apply_contextual_factors(
        BehaviorPDF& pdf,
        const TargetBehaviorTracker& tracker,
        game_object* target)
    {
        // Apply animation locks - targets are stationary during AA/spells
        if (tracker.is_animation_locked())
        {
            const auto& history = tracker.get_history();
            if (!history.empty())
            {
                // Strong bias toward current position
                pdf.add_weighted_sample(history.back().position, 2.0f);
                pdf.normalize();
            }
        }

        // Apply CS prediction - detect if enemy is moving toward low-HP minions
        if (target && target->is_valid())
        {
            std::vector<CSOpportunity> cs_opportunities = detect_cs_opportunities(target);

            // Add CS positions to PDF with moderate weight
            for (const auto& cs_opp : cs_opportunities)
            {
                // Only add to PDF if confidence is reasonable
                if (cs_opp.confidence >= 0.3f)
                {
                    // Weight based on confidence (0.3-1.0 confidence → 1.0-2.0 weight)
                    float weight = 1.0f + cs_opp.confidence;
                    pdf.add_weighted_sample(cs_opp.predicted_aa_position, weight);
                }
            }

            if (!cs_opportunities.empty())
            {
                pdf.normalize();
            }
        }
    }

    std::vector<CSOpportunity> BehaviorPredictor::detect_cs_opportunities(game_object* target)
    {
        std::vector<CSOpportunity> opportunities;

        if (!target || !target->is_valid() || !g_sdk)
        {
            return opportunities;
        }

        // Get target's AA damage (base + bonus AD)
        float target_aa_damage = target->get_attack_damage();
        float target_aa_range = target->get_attack_range();
        math::vector3 target_pos = target->get_position();

        // PHASE 1: Basic CS Detection
        // Find low-HP enemy minions within 500 units of the target
        constexpr float CS_SEARCH_RADIUS = 500.f;

        auto minion_list = g_sdk->object_manager->get_minions();
        for (auto* minion : minion_list)  // std::span can be iterated directly
        {
            if (!minion || !minion->is_valid())
                continue;

            // Only check minions on the OPPOSITE team of the target
            // (target wants to CS enemy minions, not their own)
            if (minion->get_team_id() == target->get_team_id())
                continue;

            // Check if minion is within search radius
            float distance_to_minion = target_pos.distance(minion->get_position());
            if (distance_to_minion > CS_SEARCH_RADIUS)
                continue;

            // Check if minion is low HP (killable in 2-3 autos)
            float minion_hp = minion->get_health();
            float minion_max_hp = minion->get_max_health();

            // Low HP threshold: minion can be killed in 2-3 AAs
            float cs_threshold = target_aa_damage * 3.0f;

            if (minion_hp > cs_threshold)
                continue;  // Not low enough for CS

            // CONFIDENCE CALCULATION
            // Higher confidence if:
            // 1. Minion is very low HP (1 AA away)
            // 2. Enemy is close to the minion
            // 3. Enemy is moving toward the minion

            float hp_confidence = 0.f;
            if (minion_hp <= target_aa_damage * 1.2f)
            {
                // Very low (1 AA) - HIGH confidence
                hp_confidence = 0.8f;
            }
            else if (minion_hp <= target_aa_damage * 2.0f)
            {
                // Medium low (2 AA) - MEDIUM confidence
                hp_confidence = 0.5f;
            }
            else
            {
                // 3 AA range - LOW confidence
                hp_confidence = 0.3f;
            }

            // Distance confidence: closer = more likely to CS
            float distance_confidence = 1.0f - (distance_to_minion / CS_SEARCH_RADIUS);
            distance_confidence = std::clamp(distance_confidence, 0.f, 1.f);

            // PATH AND VELOCITY DETECTION
            // Check if enemy's path or movement goes toward this minion
            float path_confidence = 0.f;

            // Check if enemy's path goes near this minion
            auto path = target->get_path();
            if (!path.empty())
            {
                // Get path endpoint (where they're walking to)
                math::vector3 path_end = path.back();
                float distance_path_to_minion = path_end.distance(minion->get_position());

                // If path endpoint is within AA range of minion, likely going to CS
                if (distance_path_to_minion <= target_aa_range + 50.f)
                {
                    path_confidence = 0.85f;  // HIGH confidence - path goes to minion
                }
                else if (distance_path_to_minion <= target_aa_range + 150.f)
                {
                    path_confidence = 0.5f;  // MEDIUM confidence - path goes near minion
                }
            }

            // Fallback: check velocity direction if no path or low path confidence
            if (path_confidence < 0.5f)
            {
                math::vector3 to_minion = minion->get_position() - target_pos;
                to_minion = to_minion.normalize();

                math::vector3 target_velocity = target->get_velocity();
                float velocity_magnitude = target_velocity.magnitude();

                if (velocity_magnitude > 10.f)  // If moving
                {
                    math::vector3 move_direction = target_velocity.normalize();
                    float dot = to_minion.dot(move_direction);

                    if (dot > 0.7f && path_confidence < 0.6f)
                        path_confidence = std::max(path_confidence, 0.6f);  // Moving toward
                    else if (dot > 0.3f && path_confidence < 0.4f)
                        path_confidence = std::max(path_confidence, 0.4f);  // Somewhat toward
                }
            }

            // If still no confidence, skip this minion
            if (path_confidence < 0.3f)
                continue;

            // Combined confidence (weighted average)
            float combined_confidence = (hp_confidence * 0.5f) +
                                       (distance_confidence * 0.2f) +
                                       (path_confidence * 0.3f);

            // CALCULATE PREDICTED AA POSITION
            // Enemy will walk to AA range of the minion
            // Position = minion_pos + (direction_to_enemy × aa_range)
            math::vector3 minion_pos = minion->get_position();
            math::vector3 from_minion_to_target = (target_pos - minion_pos).normalize();
            math::vector3 predicted_aa_pos = minion_pos + (from_minion_to_target * target_aa_range);

            // Estimate time until CS (rough approximation)
            // Average attack speed ~1.0, so ~1s per auto
            float autos_needed = minion_hp / target_aa_damage;
            float time_until_cs = autos_needed * 1.0f;  // Rough estimate: 1s per auto

            // Create CS opportunity
            CSOpportunity cs_opp;
            cs_opp.minion = minion;
            cs_opp.predicted_aa_position = predicted_aa_pos;
            cs_opp.confidence = combined_confidence;
            cs_opp.time_until_cs = time_until_cs;

            opportunities.push_back(cs_opp);
        }

        return opportunities;
    }

    // =========================================================================
    // HYBRID FUSION ENGINE IMPLEMENTATION
    // =========================================================================

    HybridPredictionResult HybridFusionEngine::compute_hybrid_prediction(
        game_object* source,
        game_object* target,
        const pred_sdk::spell_data& spell,
        TargetBehaviorTracker& tracker)
    {
        HybridPredictionResult result;

        if (!source || !target || !source->is_valid() || !target->is_valid())
        {
            result.is_valid = false;
            return result;
        }

        // Validate SDK is initialized
        if (!g_sdk)
        {
            result.is_valid = false;
            result.reasoning = "SDK not initialized";
            return result;
        }

        // =============================================================================
        // EDGE CASE DETECTION AND HANDLING
        // =============================================================================

        EdgeCases::EdgeCaseAnalysis edge_cases = EdgeCases::analyze_target(target, source, &spell);

        // Filter out invalid targets
        if (edge_cases.is_clone)
        {
            result.is_valid = false;
            result.reasoning = "Target is a clone (Shaco/Wukong/LeBlanc/Neeko)";
            return result;
        }

        if (edge_cases.blocked_by_windwall)
        {
            result.is_valid = false;
            result.reasoning = "Projectile will be blocked by windwall (Yasuo/Samira/Braum)";
            return result;
        }

        // Handle stasis (Zhonya's, GA, Bard R) - PERFECT TIMING
        if (edge_cases.stasis.is_in_stasis)
        {
            float current_time = g_sdk->clock_facade->get_game_time();
            float spell_travel_time = PhysicsPredictor::compute_arrival_time(
                source->get_position(),
                target->get_position(),
                spell.projectile_speed,
                spell.delay
            );

            float cast_delay = EdgeCases::calculate_stasis_cast_timing(
                edge_cases.stasis,
                spell_travel_time,
                current_time
            );

            if (cast_delay < 0.f)
            {
                // Can't time it properly
                result.is_valid = false;
                result.reasoning = "Stasis timing impossible - travel time too long";
                return result;
            }

            if (cast_delay > 0.f)
            {
                // Need to wait before casting
                result.is_valid = false;
                result.reasoning = "Wait " + std::to_string(cast_delay) + "s for stasis exit timing";
                return result;
            }

            // Perfect timing! Cast now for guaranteed hit on stasis exit
            result.cast_position = edge_cases.stasis.exit_position;
            result.hit_chance = 1.0f;  // 100% guaranteed
            result.physics_contribution = 1.0f;
            result.behavior_contribution = 1.0f;
            result.confidence_score = 1.0f;
            result.is_valid = true;
            result.reasoning = "STASIS EXIT PREDICTION - Spell will hit exactly when " +
                edge_cases.stasis.stasis_type + " ends. GUARANTEED HIT!";
            return result;
        }

        // Handle channeling/recall - HIGH PRIORITY STATIONARY TARGET
        if (edge_cases.channel.is_channeling || edge_cases.channel.is_recalling)
        {
            float spell_travel_time = PhysicsPredictor::compute_arrival_time(
                source->get_position(),
                target->get_position(),
                spell.projectile_speed,
                spell.delay
            );

            bool can_interrupt = EdgeCases::can_interrupt_channel(
                edge_cases.channel,
                spell_travel_time
            );

            if (!can_interrupt)
            {
                result.is_valid = false;
                result.reasoning = "Channel will finish before spell arrives";
                return result;
            }

            // Stationary target - 100% hit chance
            result.cast_position = edge_cases.channel.position;
            result.hit_chance = 1.0f;
            result.physics_contribution = 1.0f;
            result.behavior_contribution = 1.0f;
            result.confidence_score = 1.0f;
            result.is_valid = true;

            std::string action = edge_cases.channel.is_recalling ? "RECALL" : "CHANNEL";
            result.reasoning = action + " INTERRUPT - Target is stationary. GUARANTEED HIT!";
            return result;
        }

        // Handle dash prediction - ENDPOINT WITH TIMING VALIDATION (if enabled)
        if (edge_cases.dash.is_dashing && PredictionConfig::get().enable_dash_prediction)
        {
            // CRITICAL: Check if dash endpoint is within spell range
            float distance_to_dash_end = source->get_position().distance(edge_cases.dash.dash_end_position);

            if (distance_to_dash_end > spell.range)
            {
                // Dash endpoint is OUT OF RANGE - don't waste the spell
                result.is_valid = false;
                result.reasoning = "Enemy dashing OUT OF RANGE - dash endpoint too far to hit";
                return result;
            }

            float spell_travel_time = PhysicsPredictor::compute_arrival_time(
                source->get_position(),
                edge_cases.dash.dash_end_position,
                spell.projectile_speed,
                spell.delay
            );

            float current_time = g_sdk->clock_facade->get_game_time();
            bool timing_valid = EdgeCases::validate_dash_timing(
                edge_cases.dash,
                spell_travel_time,
                current_time
            );

            if (!timing_valid)
            {
                // Spell would arrive BEFORE enemy reaches dash endpoint
                // Predict at current position with very low confidence
                result.confidence_score = 0.3f;
                result.reasoning = "Enemy dashing - spell arrives before dash ends (low confidence)";
            }
            else
            {
                // Spell arrives AFTER dash ends - predict at endpoint
                // This will be used by the spell-specific prediction below
                // We'll continue to normal prediction but with dash adjustments
                result.reasoning = "Dash endpoint prediction active";
            }
        }

        // =============================================================================
        // AUTOMATIC CONE DETECTION: Check if spell has cone angle defined
        if (spell.spell_slot >= 0)
        {
            spell_entry* spell_entry_ptr = source->get_spell(spell.spell_slot);
            if (spell_entry_ptr)
            {
                auto spell_data = spell_entry_ptr->get_data();
                if (spell_data)
                {
                    auto static_data = spell_data->get_static_data();
                    if (static_data)
                    {
                        float cone_angle = static_data->get_cast_cone_angle();
                        // If spell has cone angle > 0, it's a cone spell
                        if (cone_angle > 0.f)
                        {
                            return compute_cone_prediction(source, target, spell, tracker, edge_cases);
                        }
                    }
                }
            }
        }

        // Dispatch to spell-type specific implementation based on pred_sdk type
        HybridPredictionResult spell_result;

        switch (spell.spell_type)
        {
        case pred_sdk::spell_type::linear:
            spell_result = compute_linear_prediction(source, target, spell, tracker, edge_cases);
            break;

        case pred_sdk::spell_type::circular:
            spell_result = compute_circular_prediction(source, target, spell, tracker, edge_cases);
            break;

        case pred_sdk::spell_type::targetted:
            spell_result = compute_targeted_prediction(source, target, spell, tracker, edge_cases);
            break;

        case pred_sdk::spell_type::vector:
            // Vector spells (Viktor E, Rumble R, Irelia E) - two-position optimization
            spell_result = compute_vector_prediction(source, target, spell, tracker, edge_cases);
            break;

        default:
            // Fallback to circular for unknown types
            spell_result = compute_circular_prediction(source, target, spell, tracker, edge_cases);
            break;
        }

        // Apply edge case adjustments to final result
        if (spell_result.is_valid)
        {
            // Apply confidence multipliers from edge case analysis
            spell_result.confidence_score *= edge_cases.confidence_multiplier;
            spell_result.hit_chance *= edge_cases.confidence_multiplier;

            // STATIONARY TARGET BOOST (NEW)
            // Targets standing still for 0.5s+ get minimum hitchance boost
            float stationary_boost = tracker.get_stationary_hitchance_boost(g_sdk->clock_facade->get_game_time());
            if (stationary_boost > 0.f)
            {
                // Apply minimum hitchance (not multiplier - use max)
                spell_result.hit_chance = std::max(spell_result.hit_chance, stationary_boost);
            }

            // Clamp to valid range
            spell_result.confidence_score = std::clamp(spell_result.confidence_score, 0.f, 1.f);
            spell_result.hit_chance = std::clamp(spell_result.hit_chance, 0.f, 1.f);

            // Add edge case info to reasoning
            if (edge_cases.is_slowed)
                spell_result.reasoning += "\n[SLOWED: +15% confidence]";

            if (edge_cases.has_shield)
                spell_result.reasoning += "\n[WARNING: Spell shield active - will be blocked!]";

            if (edge_cases.dash.is_dashing && !math::is_zero(edge_cases.dash.dash_end_position))
                spell_result.reasoning += "\n[DASH PREDICTION: Aiming at dash endpoint]";

            if (edge_cases.is_in_fog_of_war)
                spell_result.reasoning += "\n[FOG OF WAR: Enemy can't see us - reduced reaction time +35% confidence]";

            if (stationary_boost > 0.f)
            {
                float duration = tracker.get_stationary_duration(g_sdk->clock_facade->get_game_time());
                char boost_msg[128];
                sprintf_s(boost_msg, "\n[STATIONARY TARGET: %.1fs standing still - minimum hitchance %.0f%%]",
                    duration, stationary_boost * 100.f);
                spell_result.reasoning += boost_msg;
            }

            // CS PREDICTION LOGGING
            // Detect if target is moving toward low-HP minions (CS opportunities)
            std::vector<CSOpportunity> cs_opportunities = BehaviorPredictor::detect_cs_opportunities(target);
            if (!cs_opportunities.empty())
            {
                // Find the highest confidence CS opportunity
                float max_cs_confidence = 0.f;
                const CSOpportunity* best_cs = nullptr;

                for (const auto& cs_opp : cs_opportunities)
                {
                    if (cs_opp.confidence > max_cs_confidence)
                    {
                        max_cs_confidence = cs_opp.confidence;
                        best_cs = &cs_opp;
                    }
                }

                if (best_cs && max_cs_confidence >= 0.3f)
                {
                    char cs_msg[256];
                    sprintf_s(cs_msg, "\n[CS PREDICTION: Target moving toward low-HP minion - confidence %.0f%%, ETA %.1fs]",
                        max_cs_confidence * 100.f, best_cs->time_until_cs);
                    spell_result.reasoning += cs_msg;
                }
            }
        }

        return spell_result;
    }

    HybridPredictionResult HybridFusionEngine::compute_circular_prediction(
        game_object* source,
        game_object* target,
        const pred_sdk::spell_data& spell,
        TargetBehaviorTracker& tracker,
        const EdgeCases::EdgeCaseAnalysis& edge_cases)
    {
        HybridPredictionResult result;

        // Step 1: Compute arrival time
        float arrival_time = PhysicsPredictor::compute_arrival_time(
            source->get_position(),
            target->get_position(),
            spell.projectile_speed,
            spell.delay
        );

        // Step 2: Build reachable region (physics)
        math::vector3 target_velocity = tracker.get_current_velocity();
        float move_speed = target->get_move_speed();

        ReachableRegion reachable_region = PhysicsPredictor::compute_reachable_region(
            target->get_position(),
            target_velocity,
            arrival_time,
            move_speed
        );

        result.reachable_region = reachable_region;

        // Step 3: Build behavior PDF
        BehaviorPDF behavior_pdf = BehaviorPredictor::build_pdf_from_history(tracker, arrival_time, move_speed);
        BehaviorPredictor::apply_contextual_factors(behavior_pdf, tracker, target);

        result.behavior_pdf = behavior_pdf;

        // Step 4: Compute confidence score
        float confidence = compute_confidence_score(source, target, spell, tracker, edge_cases);
        result.confidence_score = confidence;

        // Step 5: Find optimal cast position
        math::vector3 optimal_cast_pos = find_optimal_cast_position(
            reachable_region,
            behavior_pdf,
            source->get_position(),
            spell.radius,
            confidence
        );

        result.cast_position = optimal_cast_pos;

        // Step 6: Evaluate final hit chance at optimal position
        float physics_prob = PhysicsPredictor::compute_physics_hit_probability(
            optimal_cast_pos,
            spell.radius,
            reachable_region
        );

        float behavior_prob = BehaviorPredictor::compute_behavior_hit_probability(
            optimal_cast_pos,
            spell.radius,
            behavior_pdf
        );

        result.physics_contribution = physics_prob;
        result.behavior_contribution = behavior_prob;

        // Weighted geometric fusion (trust physics more when behavior samples are sparse)
        size_t sample_count = tracker.get_history().size();
        result.hit_chance = fuse_probabilities(physics_prob, behavior_prob, confidence, sample_count);

        // Clamp to [0, 1]
        result.hit_chance = std::clamp(result.hit_chance, 0.f, 1.f);

#if HYBRID_PRED_ENABLE_REASONING
        // Generate mathematical reasoning
        std::ostringstream reasoning;
        reasoning << "Hybrid Prediction Analysis:\n";
        reasoning << "  Arrival Time: " << arrival_time << "s\n";
        reasoning << "  Reachable Radius: " << reachable_region.max_radius << " units\n";
        reasoning << "  Physics Hit Prob: " << (physics_prob * 100.f) << "%\n";
        reasoning << "  Behavior Hit Prob: " << (behavior_prob * 100.f) << "%\n";
        reasoning << "  Confidence: " << (confidence * 100.f) << "%\n";
        reasoning << "  Final HitChance: " << (result.hit_chance * 100.f) << "%\n";
        reasoning << "  Cast Position: (" << optimal_cast_pos.x << ", " << optimal_cast_pos.z << ")\n";
        result.reasoning = reasoning.str();
#else
        result.reasoning = "";
#endif

        result.is_valid = true;

        // Update opportunistic casting signals
        update_opportunity_signals(result, source, spell, tracker);

        return result;
    }

    /**
     * Check if target is in an "obvious hit" state where we should bypass normal confidence penalties
     *
     * Obvious hit states:
     * - CC'd (stunned, rooted, knocked up, etc.)
     * - Channeling (stationary abilities like Malz R, Velkoz R, etc.)
     * - Recalling (100% stationary and predictable)
     * - Walking perfectly straight (stable velocity with no direction changes)
     */
    bool is_obvious_hit(game_object* target, const TargetBehaviorTracker& tracker,
        const EdgeCases::EdgeCaseAnalysis& edge_cases)
    {
        if (!target || !target->is_valid())
            return false;

        // Check animation lock states (CC, casting, auto-attacking)
        if (tracker.is_animation_locked())
            return true;

        // Channeling or recalling = stationary = obvious hit
        if (edge_cases.channel.is_channeling || edge_cases.channel.is_recalling)
            return true;

        // Check if walking in a perfectly straight line
        // Requires at least 5 samples to establish pattern
        const auto& history = tracker.get_history();
        if (history.size() >= 5)
        {
            // Check last 5 velocity vectors for consistency
            constexpr float DIRECTION_TOLERANCE = 0.1f;  // ~5.7 degrees tolerance
            bool is_straight = true;

            math::vector3 base_direction = history[history.size() - 1].velocity;
            float base_speed = base_direction.magnitude();

            // Only consider "straight line" if actually moving
            if (base_speed > 10.f)
            {
                base_direction = base_direction / base_speed;  // Manual normalize

                for (size_t i = history.size() - 5; i < history.size() - 1; ++i)
                {
                    math::vector3 vel = history[i].velocity;
                    float speed = vel.magnitude();

                    if (speed < 10.f)
                    {
                        // Stopped moving - not straight line
                        is_straight = false;
                        break;
                    }

                    vel = vel / speed;  // Manual normalize

                    // Check angle between velocities
                    float dot = base_direction.x * vel.x + base_direction.z * vel.z;
                    if (dot < 1.0f - DIRECTION_TOLERANCE)  // ~5.7 degrees
                    {
                        is_straight = false;
                        break;
                    }
                }

                if (is_straight)
                    return true;
            }
        }

        return false;
    }

    float HybridFusionEngine::compute_confidence_score(
        game_object* source,
        game_object* target,
        const pred_sdk::spell_data& spell,
        const TargetBehaviorTracker& tracker,
        const EdgeCases::EdgeCaseAnalysis& edge_cases)
    {
        // OBVIOUS HIT OVERRIDE: Bypass all penalties for clearly hittable targets
        // This prevents confidence from strangling predictions on CC'd/channeling/straight-line targets
        if (is_obvious_hit(target, tracker, edge_cases))
        {
            return 0.95f;  // Near-perfect confidence for obvious hits
        }

        float confidence = 1.0f;

        // Distance factor - further = less confident
        float distance = (target->get_position() - source->get_position()).magnitude();
        confidence *= std::exp(-distance * CONFIDENCE_DISTANCE_DECAY);

        // Latency factor (ping in seconds)
        float ping = static_cast<float>(g_sdk->net_client->get_ping()) * 0.001f;
        confidence *= std::exp(-ping * CONFIDENCE_LATENCY_FACTOR);

        // Spell-specific adjustments
        if (spell.projectile_speed >= FLT_MAX / 2.f)
        {
            // Instant spell - higher confidence (less time for target to react)
            confidence *= 1.2f;
        }
        else if (spell.projectile_speed < 1000.f)
        {
            // Slow projectile - lower confidence (more time to dodge)
            confidence *= 0.9f;
        }

        // Mobility factor - high mobility champions are harder to predict
        float move_speed = target->get_move_speed();
        float mobility_penalty = std::clamp(move_speed / 500.f, 0.5f, 1.5f);

        // Safe to divide - clamp guarantees [0.5, 1.5] range (never zero)
        confidence /= mobility_penalty;

        // Sample size factor - more data = more confidence
        const auto& history = tracker.get_history();
        if (history.size() < MIN_SAMPLES_FOR_BEHAVIOR)
        {
            confidence *= static_cast<float>(history.size()) / MIN_SAMPLES_FOR_BEHAVIOR;
        }

        // Animation lock boost - target is locked in animation
        // Multiplicative (not additive) to preserve relative confidence levels
        if (tracker.is_animation_locked())
        {
            confidence *= (1.0f + ANIMATION_LOCK_CONFIDENCE_BOOST);  // 1.3x multiplier
        }

        return std::clamp(confidence, 0.1f, 1.0f);
    }

    math::vector3 HybridFusionEngine::find_optimal_cast_position(
        const ReachableRegion& reachable_region,
        const BehaviorPDF& behavior_pdf,
        const math::vector3& source_pos,
        float projectile_radius,
        float confidence)
    {
        // Grid search over reachable region
        constexpr int GRID_SEARCH_SIZE = 16;
        float best_score = -1.f;
        math::vector3 best_position = reachable_region.center;

        float search_radius = reachable_region.max_radius;
        float step = search_radius * 2.f / GRID_SEARCH_SIZE;

        for (int i = 0; i < GRID_SEARCH_SIZE; ++i)
        {
            for (int j = 0; j < GRID_SEARCH_SIZE; ++j)
            {
                math::vector3 test_pos = reachable_region.center;
                test_pos.x += (i - GRID_SEARCH_SIZE / 2) * step;
                test_pos.z += (j - GRID_SEARCH_SIZE / 2) * step;

                float score = evaluate_hit_chance_at_point(
                    test_pos,
                    reachable_region,
                    behavior_pdf,
                    projectile_radius,
                    confidence
                );

                if (score > best_score)
                {
                    best_score = score;
                    best_position = test_pos;
                }
            }
        }

        // Gradient ascent refinement (2 iterations)
        for (int iter = 0; iter < 2; ++iter)
        {
            constexpr float GRADIENT_STEP = 10.f;
            constexpr int GRADIENT_SAMPLES = 8;

            math::vector3 gradient{};

            for (int i = 0; i < GRADIENT_SAMPLES; ++i)
            {
                float angle = (2.f * PI * i) / GRADIENT_SAMPLES;
                math::vector3 test_pos = best_position;
                test_pos.x += GRADIENT_STEP * std::cos(angle);
                test_pos.z += GRADIENT_STEP * std::sin(angle);

                float score = evaluate_hit_chance_at_point(
                    test_pos,
                    reachable_region,
                    behavior_pdf,
                    projectile_radius,
                    confidence
                );

                float angle_weight = score - best_score;
                gradient.x += angle_weight * std::cos(angle);
                gradient.z += angle_weight * std::sin(angle);
            }

            if (gradient.magnitude() > EPSILON)
            {
                best_position = best_position + gradient.normalized() * GRADIENT_STEP * 0.5f;
                best_score = evaluate_hit_chance_at_point(
                    best_position,
                    reachable_region,
                    behavior_pdf,
                    projectile_radius,
                    confidence
                );
            }
        }

        return best_position;
    }

    float HybridFusionEngine::evaluate_hit_chance_at_point(
        const math::vector3& point,
        const ReachableRegion& reachable_region,
        const BehaviorPDF& behavior_pdf,
        float projectile_radius,
        float confidence)
    {
        float physics_prob = PhysicsPredictor::compute_physics_hit_probability(
            point,
            projectile_radius,
            reachable_region
        );

        float behavior_prob = BehaviorPredictor::compute_behavior_hit_probability(
            point,
            projectile_radius,
            behavior_pdf
        );

        return physics_prob * behavior_prob * confidence;
    }

    // =========================================================================
    // SPELL-TYPE SPECIFIC IMPLEMENTATIONS
    // =========================================================================

    HybridPredictionResult HybridFusionEngine::compute_linear_prediction(
        game_object* source,
        game_object* target,
        const pred_sdk::spell_data& spell,
        TargetBehaviorTracker& tracker,
        const EdgeCases::EdgeCaseAnalysis& edge_cases)
    {
        HybridPredictionResult result;

        if (!source || !target || !source->is_valid() || !target->is_valid())
        {
            result.is_valid = false;
            return result;
        }

        // Validate SDK is initialized
        if (!g_sdk)
        {
            result.is_valid = false;
            result.reasoning = "SDK not initialized";
            return result;
        }

        // Step 1: Compute arrival time
        float arrival_time = PhysicsPredictor::compute_arrival_time(
            source->get_position(),
            target->get_position(),
            spell.projectile_speed,
            spell.delay
        );

        // Step 2: Build reachable region (physics)
        math::vector3 target_velocity = tracker.get_current_velocity();
        float move_speed = target->get_move_speed();

        ReachableRegion reachable_region = PhysicsPredictor::compute_reachable_region(
            target->get_position(),
            target_velocity,
            arrival_time,
            move_speed
        );

        result.reachable_region = reachable_region;

        // Step 3: Build behavior PDF
        BehaviorPDF behavior_pdf = BehaviorPredictor::build_pdf_from_history(tracker, arrival_time, move_speed);
        BehaviorPredictor::apply_contextual_factors(behavior_pdf, tracker, target);

        result.behavior_pdf = behavior_pdf;

        // Step 4: Compute confidence score
        float confidence = compute_confidence_score(source, target, spell, tracker, edge_cases);
        result.confidence_score = confidence;

        // Step 5: Compute capsule parameters
        // Linear spell = capsule from source toward target
        math::vector3 to_target = target->get_position() - source->get_position();
        float dist_to_target = to_target.magnitude();

        constexpr float MIN_SAFE_DISTANCE = 1.0f;  // Minimum safe distance for normalization
        if (dist_to_target < MIN_SAFE_DISTANCE)
        {
            result.is_valid = false;
            result.reasoning = "Target too close - zero distance";
            return result;
        }

        math::vector3 direction = to_target / dist_to_target;  // Safe manual normalize
        math::vector3 capsule_start = source->get_position();
        float capsule_length = spell.range;
        float capsule_radius = spell.radius;

        // For linear spells, cast position is the source position (spell shoots in direction)
        // But we need to find optimal direction
        math::vector3 to_center = reachable_region.center - source->get_position();
        float dist_to_center = to_center.magnitude();

        math::vector3 optimal_direction;
        if (dist_to_center > MIN_SAFE_DISTANCE)
        {
            optimal_direction = to_center / dist_to_center;  // Safe manual normalize
        }
        else
        {
            optimal_direction = direction;  // Fallback to target direction
        }

        result.cast_position = source->get_position() + optimal_direction * capsule_length;

        // Step 6: Compute hit probabilities for capsule
        float physics_prob = compute_capsule_reachability_overlap(
            capsule_start,
            optimal_direction,
            capsule_length,
            capsule_radius,
            reachable_region
        );

        float behavior_prob = compute_capsule_behavior_probability(
            capsule_start,
            optimal_direction,
            capsule_length,
            capsule_radius,
            behavior_pdf
        );

        result.physics_contribution = physics_prob;
        result.behavior_contribution = behavior_prob;

        // Weighted geometric fusion (trust physics more when behavior samples are sparse)
        size_t sample_count = tracker.get_history().size();
        result.hit_chance = fuse_probabilities(physics_prob, behavior_prob, confidence, sample_count);
        result.hit_chance = std::clamp(result.hit_chance, 0.f, 1.f);

#if HYBRID_PRED_ENABLE_REASONING
        // Generate mathematical reasoning
        std::ostringstream reasoning;
        reasoning << "Hybrid Prediction Analysis (LINEAR):\n";
        reasoning << "  Arrival Time: " << arrival_time << "s\n";
        reasoning << "  Reachable Radius: " << reachable_region.max_radius << " units\n";
        reasoning << "  Capsule Length: " << capsule_length << " units\n";
        reasoning << "  Capsule Width: " << (capsule_radius * 2.f) << " units\n";
        reasoning << "  Physics Hit Prob: " << (physics_prob * 100.f) << "%\n";
        reasoning << "  Behavior Hit Prob: " << (behavior_prob * 100.f) << "%\n";
        reasoning << "  Confidence: " << (confidence * 100.f) << "%\n";
        reasoning << "  Final HitChance: " << (result.hit_chance * 100.f) << "%\n";
        result.reasoning = reasoning.str();
#else
        result.reasoning = "";
#endif

        result.is_valid = true;

        // Update opportunistic casting signals
        update_opportunity_signals(result, source, spell, tracker);

        return result;
    }

    HybridPredictionResult HybridFusionEngine::compute_targeted_prediction(
        game_object* source,
        game_object* target,
        const pred_sdk::spell_data& spell,
        TargetBehaviorTracker& tracker,
        const EdgeCases::EdgeCaseAnalysis& edge_cases)
    {
        (void)spell;  // Targeted spells ignore most spell parameters

        HybridPredictionResult result;

        if (!source || !target || !source->is_valid() || !target->is_valid())
        {
            result.is_valid = false;
            return result;
        }

        // Validate SDK is initialized
        if (!g_sdk)
        {
            result.is_valid = false;
            result.reasoning = "SDK not initialized";
            return result;
        }

        // Targeted spells can't miss (unless target becomes untargetable)
        result.cast_position = target->get_position();
        result.hit_chance = 1.0f;
        result.physics_contribution = 1.0f;
        result.behavior_contribution = 1.0f;
        result.confidence_score = compute_confidence_score(source, target, spell, tracker, edge_cases);
        result.is_valid = true;
        result.reasoning = "Targeted spell - guaranteed hit (unless target becomes untargetable)";

        return result;
    }

    HybridPredictionResult HybridFusionEngine::compute_vector_prediction(
        game_object* source,
        game_object* target,
        const pred_sdk::spell_data& spell,
        TargetBehaviorTracker& tracker,
        const EdgeCases::EdgeCaseAnalysis& edge_cases)
    {
        HybridPredictionResult result;

        if (!source || !target || !source->is_valid() || !target->is_valid())
        {
            result.is_valid = false;
            return result;
        }

        // Validate SDK is initialized
        if (!g_sdk)
        {
            result.is_valid = false;
            result.reasoning = "SDK not initialized";
            return result;
        }

        // Step 1: Compute arrival time
        float arrival_time = PhysicsPredictor::compute_arrival_time(
            source->get_position(),
            target->get_position(),
            spell.projectile_speed,
            spell.delay
        );

        // Step 2: Build reachable region (physics)
        math::vector3 target_velocity = tracker.get_current_velocity();
        float move_speed = target->get_move_speed();

        ReachableRegion reachable_region = PhysicsPredictor::compute_reachable_region(
            target->get_position(),
            target_velocity,
            arrival_time,
            move_speed
        );

        result.reachable_region = reachable_region;

        // Step 3: Build behavior PDF
        BehaviorPDF behavior_pdf = BehaviorPredictor::build_pdf_from_history(tracker, arrival_time, move_speed);
        BehaviorPredictor::apply_contextual_factors(behavior_pdf, tracker, target);

        result.behavior_pdf = behavior_pdf;

        // Step 4: Compute confidence score
        float confidence = compute_confidence_score(source, target, spell, tracker, edge_cases);
        result.confidence_score = confidence;

        // Step 5: Optimize vector orientation
        // Test multiple orientations to find best two-position configuration
        size_t sample_count = tracker.get_history().size();
        VectorConfiguration best_config = optimize_vector_orientation(
            source,
            reachable_region.center,
            reachable_region,
            behavior_pdf,
            spell,
            confidence,
            sample_count
        );

        // Step 6: Set result from best configuration
        result.first_cast_position = best_config.first_cast_position;
        result.cast_position = best_config.cast_position;
        result.physics_contribution = best_config.physics_prob;
        result.behavior_contribution = best_config.behavior_prob;
        result.hit_chance = best_config.hit_chance;

#if HYBRID_PRED_ENABLE_REASONING
        // Generate mathematical reasoning
        std::ostringstream reasoning;
        reasoning << "Hybrid Prediction Analysis (VECTOR):\n";
        reasoning << "  Arrival Time: " << arrival_time << "s\n";
        reasoning << "  Reachable Radius: " << reachable_region.max_radius << " units\n";
        reasoning << "  Vector Length: " << spell.range << " units\n";
        reasoning << "  Vector Width: " << (spell.radius * 2.f) << " units\n";
        reasoning << "  First Cast: (" << best_config.first_cast_position.x << ", "
            << best_config.first_cast_position.z << ")\n";
        reasoning << "  Second Cast: (" << best_config.cast_position.x << ", "
            << best_config.cast_position.z << ")\n";
        reasoning << "  Physics Hit Prob: " << (best_config.physics_prob * 100.f) << "%\n";
        reasoning << "  Behavior Hit Prob: " << (best_config.behavior_prob * 100.f) << "%\n";
        reasoning << "  Confidence: " << (confidence * 100.f) << "%\n";
        reasoning << "  Final HitChance: " << (result.hit_chance * 100.f) << "%\n";
        result.reasoning = reasoning.str();
#else
        result.reasoning = "";
#endif

        result.is_valid = true;

        // Update opportunistic casting signals
        update_opportunity_signals(result, source, spell, tracker);

        return result;
    }

    HybridPredictionResult HybridFusionEngine::compute_cone_prediction(
        game_object* source,
        game_object* target,
        const pred_sdk::spell_data& spell,
        TargetBehaviorTracker& tracker,
        const EdgeCases::EdgeCaseAnalysis& edge_cases)
    {
        HybridPredictionResult result;

        if (!source || !target || !source->is_valid() || !target->is_valid())
        {
            result.is_valid = false;
            return result;
        }

        // Validate SDK is initialized
        if (!g_sdk)
        {
            result.is_valid = false;
            result.reasoning = "SDK not initialized";
            return result;
        }

        // Step 1: Compute arrival time (instant for most cone spells)
        float arrival_time = PhysicsPredictor::compute_arrival_time(
            source->get_position(),
            target->get_position(),
            spell.projectile_speed,
            spell.delay
        );

        // Step 2: Build reachable region (physics)
        math::vector3 target_velocity = tracker.get_current_velocity();
        float move_speed = target->get_move_speed();

        ReachableRegion reachable_region = PhysicsPredictor::compute_reachable_region(
            target->get_position(),
            target_velocity,
            arrival_time,
            move_speed
        );

        result.reachable_region = reachable_region;

        // Step 3: Build behavior PDF
        BehaviorPDF behavior_pdf = BehaviorPredictor::build_pdf_from_history(tracker, arrival_time, move_speed);
        BehaviorPredictor::apply_contextual_factors(behavior_pdf, tracker, target);

        result.behavior_pdf = behavior_pdf;

        // Step 4: Compute confidence score
        float confidence = compute_confidence_score(source, target, spell, tracker, edge_cases);
        result.confidence_score = confidence;

        // Step 5: Compute cone parameters
        // TODO: CONE ANGLE INTERPRETATION IS AMBIGUOUS
        // Current assumption: spell.radius = "width at range", so half-angle = atan2(radius, range)
        // BUT SDK might encode differently:
        //   - Option 1: spell.radius = total cone spread in degrees (e.g., Annie W = 50°)
        //   - Option 2: spell.radius = half-angle already (e.g., Annie W = 25°)
        //   - Option 3: spell.radius = width at max range (current assumption)
        // REQUIRES EMPIRICAL TESTING with known cone spells (Annie W, Cassio R, etc.)
        float cone_half_angle = std::atan2(spell.radius, spell.range);
        float cone_range = spell.range;

        // Optimal direction toward predicted target position
        math::vector3 to_center = reachable_region.center - source->get_position();
        float dist_to_center = to_center.magnitude();

        constexpr float MIN_SAFE_DISTANCE = 1.0f;  // Minimum safe distance for normalization
        if (dist_to_center < MIN_SAFE_DISTANCE)
        {
            result.is_valid = false;
            result.reasoning = "Target too close - zero distance";
            return result;
        }

        math::vector3 direction = to_center / dist_to_center;  // Safe manual normalize
        result.cast_position = source->get_position() + direction * cone_range;

        // Step 6: Compute hit probabilities for cone
        float physics_prob = compute_cone_reachability_overlap(
            source->get_position(),
            direction,
            cone_half_angle,
            cone_range,
            reachable_region
        );

        float behavior_prob = compute_cone_behavior_probability(
            source->get_position(),
            direction,
            cone_half_angle,
            cone_range,
            behavior_pdf
        );

        result.physics_contribution = physics_prob;
        result.behavior_contribution = behavior_prob;

        // Weighted geometric fusion (trust physics more when behavior samples are sparse)
        size_t sample_count = tracker.get_history().size();
        result.hit_chance = fuse_probabilities(physics_prob, behavior_prob, confidence, sample_count);
        result.hit_chance = std::clamp(result.hit_chance, 0.f, 1.f);

#if HYBRID_PRED_ENABLE_REASONING
        // Generate mathematical reasoning
        std::ostringstream reasoning;
        reasoning << "Hybrid Prediction Analysis (CONE):\n";
        reasoning << "  Arrival Time: " << arrival_time << "s\n";
        reasoning << "  Reachable Radius: " << reachable_region.max_radius << " units\n";
        reasoning << "  Cone Range: " << cone_range << " units\n";
        reasoning << "  Cone Half-Angle: " << (cone_half_angle * 180.f / PI) << " degrees\n";
        reasoning << "  Physics Hit Prob: " << (physics_prob * 100.f) << "%\n";
        reasoning << "  Behavior Hit Prob: " << (behavior_prob * 100.f) << "%\n";
        reasoning << "  Confidence: " << (confidence * 100.f) << "%\n";
        reasoning << "  Final HitChance: " << (result.hit_chance * 100.f) << "%\n";
        result.reasoning = reasoning.str();
#else
        result.reasoning = "";
#endif

        result.is_valid = true;

        // Update opportunistic casting signals
        update_opportunity_signals(result, source, spell, tracker);

        return result;
    }

    // =========================================================================
    // GEOMETRY HELPERS FOR SPELL SHAPES
    // =========================================================================

    bool HybridFusionEngine::point_in_capsule(
        const math::vector3& point,
        const math::vector3& capsule_start,
        const math::vector3& capsule_end,
        float capsule_radius)
    {
        // Capsule = line segment + radius
        // Point is inside if distance to line segment ≤ radius

        math::vector3 segment = capsule_end - capsule_start;
        math::vector3 to_point = point - capsule_start;

        float segment_length_sq = segment.x * segment.x + segment.z * segment.z;

        if (segment_length_sq < EPSILON)
        {
            // Degenerate case: start == end (treat as sphere)
            float dist_sq = to_point.x * to_point.x + to_point.z * to_point.z;
            return dist_sq <= capsule_radius * capsule_radius;
        }

        // Project point onto line segment (clamped to [0, 1])
        float t = (to_point.x * segment.x + to_point.z * segment.z) / segment_length_sq;
        t = std::clamp(t, 0.f, 1.f);

        // Closest point on segment
        math::vector3 closest = capsule_start + segment * t;

        // Distance from point to closest point on segment
        float dx = point.x - closest.x;
        float dz = point.z - closest.z;
        float dist_sq = dx * dx + dz * dz;

        return dist_sq <= capsule_radius * capsule_radius;
    }

    float HybridFusionEngine::compute_capsule_reachability_overlap(
        const math::vector3& capsule_start,
        const math::vector3& capsule_direction,
        float capsule_length,
        float capsule_radius,
        const ReachableRegion& reachable_region)
    {
        // Quasi-Monte Carlo integration: Sample points in reachable circle using Fermat spiral,
        // count how many fall inside capsule (deterministic low-discrepancy sampling)

        if (reachable_region.area < EPSILON)
            return 0.f;

        math::vector3 capsule_end = capsule_start + capsule_direction * capsule_length;

        constexpr int SAMPLES = 128;
        constexpr float SPIRAL_FACTOR = 7.f;  // Coprime with SAMPLES for uniform coverage
        int hits = 0;

        for (int i = 0; i < SAMPLES; ++i)
        {
            // Fermat spiral: uniform area distribution in reachable disk
            float r = reachable_region.max_radius * std::sqrt(static_cast<float>(i) / SAMPLES);
            float theta = (2.f * PI * i) / SAMPLES * SPIRAL_FACTOR;  // 7 is coprime with 128

            math::vector3 sample_point = reachable_region.center;
            sample_point.x += r * std::cos(theta);
            sample_point.z += r * std::sin(theta);

            if (point_in_capsule(sample_point, capsule_start, capsule_end, capsule_radius))
            {
                ++hits;
            }
        }

        return static_cast<float>(hits) / SAMPLES;
    }

    float HybridFusionEngine::compute_capsule_behavior_probability(
        const math::vector3& capsule_start,
        const math::vector3& capsule_direction,
        float capsule_length,
        float capsule_radius,
        const BehaviorPDF& pdf)
    {
        if (pdf.total_probability < EPSILON)
            return 1.0f;  // Neutral fallback

        // Direct grid summation (more accurate than sampling)
        // Sum probability mass of all cells whose centers fall inside the capsule
        math::vector3 capsule_end = capsule_start + capsule_direction * capsule_length;
        float prob = 0.f;

        for (int x = 0; x < BehaviorPDF::GRID_SIZE; ++x)
        {
            for (int z = 0; z < BehaviorPDF::GRID_SIZE; ++z)
            {
                // World position of cell center
                float wx = pdf.origin.x + (x - BehaviorPDF::GRID_SIZE / 2 + 0.5f) * pdf.cell_size;
                float wz = pdf.origin.z + (z - BehaviorPDF::GRID_SIZE / 2 + 0.5f) * pdf.cell_size;
                math::vector3 cell_center(wx, pdf.origin.y, wz);

                // Check if cell center is inside capsule
                if (point_in_capsule(cell_center, capsule_start, capsule_end, capsule_radius))
                {
                    prob += pdf.pdf_grid[x][z];
                }
            }
        }

        // PDF is normalized (sums to 1), so this sum is the exact hit probability
        return std::clamp(prob, 0.f, 1.f);
    }

    // =========================================================================
    // CONE GEOMETRY HELPERS
    // =========================================================================

    bool HybridFusionEngine::point_in_cone(
        const math::vector3& point,
        const math::vector3& cone_origin,
        const math::vector3& cone_direction,
        float cone_half_angle,
        float cone_range)
    {
        // Cone = circular sector in 3D
        // Point is inside if:
        // 1. Distance from origin ≤ range
        // 2. Angle from cone axis ≤ half_angle

        math::vector3 to_point = point - cone_origin;
        float distance_sq = to_point.x * to_point.x + to_point.z * to_point.z;

        // Check range
        if (distance_sq > cone_range * cone_range)
            return false;

        float distance = std::sqrt(distance_sq);
        if (distance < EPSILON)
            return true;  // At origin

        // Check angle
        // cos(angle) = dot(to_point, cone_direction) / |to_point|
        float dot_product = to_point.x * cone_direction.x + to_point.z * cone_direction.z;
        float cos_angle = dot_product / distance;

        // cos decreases as angle increases, so:
        // angle ≤ half_angle  ⇔  cos(angle) ≥ cos(half_angle)
        float cos_half_angle = std::cos(cone_half_angle);

        return cos_angle >= cos_half_angle;
    }

    float HybridFusionEngine::compute_cone_reachability_overlap(
        const math::vector3& cone_origin,
        const math::vector3& cone_direction,
        float cone_half_angle,
        float cone_range,
        const ReachableRegion& reachable_region)
    {
        // Quasi-Monte Carlo integration: Sample points in reachable circle using Fermat spiral,
        // count how many fall inside cone (deterministic low-discrepancy sampling)

        if (reachable_region.area < EPSILON)
            return 0.f;

        constexpr int SAMPLES = 128;
        constexpr float SPIRAL_FACTOR = 7.f;  // Coprime with SAMPLES for uniform coverage
        int hits = 0;

        for (int i = 0; i < SAMPLES; ++i)
        {
            // Fermat spiral: uniform area distribution in reachable disk
            float r = reachable_region.max_radius * std::sqrt(static_cast<float>(i) / SAMPLES);
            float theta = (2.f * PI * i) / SAMPLES * SPIRAL_FACTOR;  // 7 is coprime with 128

            math::vector3 sample_point = reachable_region.center;
            sample_point.x += r * std::cos(theta);
            sample_point.z += r * std::sin(theta);

            if (point_in_cone(sample_point, cone_origin, cone_direction, cone_half_angle, cone_range))
            {
                ++hits;
            }
        }

        return static_cast<float>(hits) / SAMPLES;
    }

    float HybridFusionEngine::compute_cone_behavior_probability(
        const math::vector3& cone_origin,
        const math::vector3& cone_direction,
        float cone_half_angle,
        float cone_range,
        const BehaviorPDF& pdf)
    {
        if (pdf.total_probability < EPSILON)
            return 1.0f;  // Neutral fallback

        // Direct grid summation (more accurate than sampling)
        // Sum probability mass of all cells whose centers fall inside the cone
        float prob = 0.f;

        for (int x = 0; x < BehaviorPDF::GRID_SIZE; ++x)
        {
            for (int z = 0; z < BehaviorPDF::GRID_SIZE; ++z)
            {
                // World position of cell center
                float wx = pdf.origin.x + (x - BehaviorPDF::GRID_SIZE / 2 + 0.5f) * pdf.cell_size;
                float wz = pdf.origin.z + (z - BehaviorPDF::GRID_SIZE / 2 + 0.5f) * pdf.cell_size;
                math::vector3 cell_center(wx, pdf.origin.y, wz);

                // Check if cell center is inside cone
                if (point_in_cone(cell_center, cone_origin, cone_direction, cone_half_angle, cone_range))
                {
                    prob += pdf.pdf_grid[x][z];
                }
            }
        }

        // PDF is normalized (sums to 1), so this sum is the exact hit probability
        return std::clamp(prob, 0.f, 1.f);
    }

    // =========================================================================
    // VECTOR SPELL OPTIMIZATION HELPERS
    // =========================================================================

    HybridFusionEngine::VectorConfiguration HybridFusionEngine::optimize_vector_orientation(
        game_object* source,
        const math::vector3& predicted_target_pos,
        const ReachableRegion& reachable_region,
        const BehaviorPDF& behavior_pdf,
        const pred_sdk::spell_data& spell,
        float confidence,
        size_t sample_count)
    {
        /**
         * Vector Spell Optimization Algorithm
         * ====================================
         *
         * Goal: Find optimal two-position configuration (first_cast, cast_position)
         * that maximizes hit_chance against predicted target
         *
         * Constraints:
         * - first_cast must be within spell.cast_range of source
         * - Vector line length = spell.range (from first_cast to cast_position)
         * - Line passes through or near predicted_target_pos
         *
         * Method:
         * 1. Test multiple orientations (20 angles from 0-360°)
         * 2. For each orientation:
         *    a. Position line centered on predicted target
         *    b. Compute first_cast and cast_position
         *    c. Verify first_cast is within cast_range
         *    d. Compute hit probability using capsule geometry
         * 3. Return configuration with highest hit_chance
         */

        VectorConfiguration best_config;
        best_config.hit_chance = 0.f;

        math::vector3 source_pos = source->get_position();
        float vector_length = spell.range;
        float vector_width = spell.radius;
        float max_first_cast_range = spell.cast_range;

        // If cast_range is 0, use range as default (some spells don't set cast_range)
        if (max_first_cast_range < EPSILON)
            max_first_cast_range = spell.range;

        // Precompute distance to predicted target for normalization check
        math::vector3 to_predicted = predicted_target_pos - source_pos;
        float dist_to_predicted = to_predicted.magnitude();

        // Test multiple orientations
        constexpr int NUM_ORIENTATIONS = 20;

        for (int i = 0; i < NUM_ORIENTATIONS; ++i)
        {
            float angle = (2.f * PI * i) / NUM_ORIENTATIONS;
            math::vector3 direction(std::cos(angle), 0.f, std::sin(angle));

            // Position vector line centered on predicted target
            // Line goes from (target - dir*length/2) to (target + dir*length/2)
            math::vector3 first_cast = predicted_target_pos - direction * (vector_length * 0.5f);
            math::vector3 second_cast = predicted_target_pos + direction * (vector_length * 0.5f);

            // Check if first_cast is within range from source
            float distance_to_first_cast = (first_cast - source_pos).magnitude();
            if (distance_to_first_cast > max_first_cast_range)
            {
                // Adjust: Move line closer to source while maintaining orientation
                // Place first_cast at max_first_cast_range in direction of predicted target
                if (dist_to_predicted > EPSILON)
                {
                    math::vector3 to_target = to_predicted / dist_to_predicted;  // Safe manual normalize
                    first_cast = source_pos + to_target * max_first_cast_range;
                    second_cast = first_cast + direction * vector_length;
                }
                else
                {
                    // Target too close - use default forward direction
                    first_cast = source_pos + direction * std::min(max_first_cast_range, vector_length * 0.5f);
                    second_cast = first_cast + direction * vector_length;
                }
            }

            // Compute hit probability for this configuration
            float physics_prob = compute_capsule_reachability_overlap(
                first_cast,
                direction,
                vector_length,
                vector_width,
                reachable_region
            );

            float behavior_prob = compute_capsule_behavior_probability(
                first_cast,
                direction,
                vector_length,
                vector_width,
                behavior_pdf
            );

            // Weighted geometric fusion (trust physics more when behavior samples are sparse)
            float hit_chance = fuse_probabilities(physics_prob, behavior_prob, confidence, sample_count);

            // Update best configuration
            if (hit_chance > best_config.hit_chance)
            {
                best_config.first_cast_position = first_cast;
                best_config.cast_position = second_cast;
                best_config.hit_chance = hit_chance;
                best_config.physics_prob = physics_prob;
                best_config.behavior_prob = behavior_prob;
            }
        }

        // If no valid configuration found, use default (aim at target)
        if (best_config.hit_chance < EPSILON)
        {
            math::vector3 direction_to_target;
            if (dist_to_predicted > EPSILON)
            {
                direction_to_target = to_predicted / dist_to_predicted;  // Safe manual normalize
            }
            else
            {
                // Target too close - use forward direction (along x-axis)
                direction_to_target = math::vector3(1.f, 0.f, 0.f);
            }

            best_config.first_cast_position = source_pos + direction_to_target * std::min(max_first_cast_range, vector_length * 0.5f);
            best_config.cast_position = best_config.first_cast_position + direction_to_target * vector_length;
            best_config.hit_chance = 0.1f;  // Low default
            best_config.physics_prob = 0.1f;
            best_config.behavior_prob = 1.0f;
        }

        return best_config;
    }

    // =========================================================================
    // PREDICTION MANAGER IMPLEMENTATION
    // =========================================================================

    void PredictionManager::update()
    {
        float current_time = g_sdk->clock_facade->get_game_time();

        // Update all existing trackers
        for (auto& pair : trackers_)
        {
            if (pair.second)
            {
                pair.second->update();
            }
        }

        // Clean up invalid trackers
        for (auto it = trackers_.begin(); it != trackers_.end(); )
        {
            bool should_remove = false;

            if (!it->second)
            {
                // Null pointer - immediate removal
                should_remove = true;
            }
            else
            {
                // Attempt to get target from object manager
                game_object* target = g_sdk->object_manager->get_object_by_network_id(it->first);

                const auto& history = it->second->get_history();

                if (!target)
                {
                    // Target no longer exists (died, recalled, DC'd)
                    // Keep tracker for TRACKER_TIMEOUT in case they respawn/reconnect
                    if (history.empty())
                    {
                        should_remove = true;  // Never collected data
                    }
                    else
                    {
                        float time_since_last = current_time - history.back().timestamp;
                        if (time_since_last > TRACKER_TIMEOUT)
                        {
                            should_remove = true;
                        }
                    }
                }
                else
                {
                    // Target exists - keep tracker
                    // Even if in fog of war, preserve learned patterns
                    should_remove = false;
                }
            }

            if (should_remove)
            {
                it = trackers_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        last_update_time_ = current_time;
    }

    TargetBehaviorTracker* PredictionManager::get_tracker(game_object* target)
    {
        if (!target || !target->is_valid())
            return nullptr;

        uint32_t network_id = target->get_network_id();

        auto it = trackers_.find(network_id);
        if (it != trackers_.end())
        {
            return it->second.get();
        }

        // Create new tracker
        auto tracker = std::make_unique<TargetBehaviorTracker>(target);
        auto* tracker_ptr = tracker.get();
        trackers_[network_id] = std::move(tracker);

        return tracker_ptr;
    }

    HybridPredictionResult PredictionManager::predict(
        game_object* source,
        game_object* target,
        const pred_sdk::spell_data& spell)
    {
        auto* tracker = get_tracker(target);
        if (!tracker)
        {
            HybridPredictionResult result;
            result.is_valid = false;
            return result;
        }

        return HybridFusionEngine::compute_hybrid_prediction(source, target, spell, *tracker);
    }

    void PredictionManager::clear()
    {
        trackers_.clear();
    }

} // namespace HybridPred