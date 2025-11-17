#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <windows.h>

/**
 * =============================================================================
 * PREDICTION TELEMETRY SYSTEM
 * =============================================================================
 *
 * Tracks prediction performance metrics for post-game analysis.
 * Logs to file: dannypred_telemetry_TIMESTAMP.txt
 *
 * =============================================================================
 */

namespace PredictionTelemetry
{
    struct PredictionEvent
    {
        float timestamp;
        std::string target_name;
        std::string spell_type;
        float hit_chance;
        float confidence;
        float distance;
        bool was_dash;
        bool was_stationary;
        bool was_animation_locked;
        bool collision_detected;
        float computation_time_ms;
        std::string edge_case;  // "stasis", "channeling", "dash", "normal"
    };

    struct SessionStats
    {
        // Prediction counts
        int total_predictions = 0;
        int valid_predictions = 0;
        int invalid_predictions = 0;

        // Edge case counts
        int dash_predictions = 0;
        int stasis_predictions = 0;
        int channel_predictions = 0;
        int stationary_predictions = 0;
        int animation_lock_predictions = 0;

        // Collision stats
        int collision_detections = 0;
        int ally_collisions = 0;
        int enemy_collisions = 0;

        // Performance
        float total_computation_time_ms = 0.f;
        float max_computation_time_ms = 0.f;
        float min_computation_time_ms = 999999.f;

        // Hitchance distribution
        int hitchance_0_20 = 0;   // 0-20%
        int hitchance_20_40 = 0;  // 20-40%
        int hitchance_40_60 = 0;  // 40-60%
        int hitchance_60_80 = 0;  // 60-80%
        int hitchance_80_100 = 0; // 80-100%

        // Per-spell-type stats
        std::unordered_map<std::string, int> spell_type_counts;
        std::unordered_map<std::string, float> spell_type_avg_hitchance;

        // Per-target stats
        std::unordered_map<std::string, int> target_prediction_counts;
        std::unordered_map<std::string, float> target_avg_hitchance;

        // Pattern detection
        int patterns_detected = 0;
        int alternating_patterns = 0;
        int repeating_patterns = 0;

        // Session info
        std::string session_start_time;
        std::string champion_name;
        float session_duration_seconds = 0.f;
    };

    class TelemetryLogger
    {
    private:
        static SessionStats stats_;
        static std::vector<PredictionEvent> events_;
        static bool enabled_;
        static std::string log_file_path_;

        static auto get_timestamp()
        {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            std::tm tm;
            localtime_s(&tm, &time);

            std::ostringstream oss;
            oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
            return oss.str();
        }

    public:
        static void initialize(const std::string& champion_name, bool enable = true)
        {
            enabled_ = enable;
            if (!enabled_) return;

            stats_ = SessionStats();
            events_.clear();

            stats_.session_start_time = get_timestamp();
            stats_.champion_name = champion_name;

            // Create log file path with full path
            char current_dir[MAX_PATH];
            GetCurrentDirectoryA(MAX_PATH, current_dir);
            log_file_path_ = std::string(current_dir) + "\\dannypred_telemetry_" + stats_.session_start_time + ".txt";

            // Write header
            std::ofstream file(log_file_path_, std::ios::app);
            if (file.is_open())
            {
                file << "=============================================================================\n";
                file << "Danny Prediction SDK - Telemetry Log\n";
                file << "=============================================================================\n";
                file << "Champion: " << champion_name << "\n";
                file << "Session Start: " << stats_.session_start_time << "\n";
                file << "=============================================================================\n\n";
                file.close();

                // Log file creation to console
                if (g_sdk)
                {
                    std::string msg = "[Danny.Prediction] Telemetry file created: " + log_file_path_;
                    g_sdk->log_console(msg.c_str());
                }
            }
            else
            {
                // Failed to create file
                if (g_sdk)
                {
                    std::string msg = "[Danny.Prediction] ERROR: Failed to create telemetry file: " + log_file_path_;
                    g_sdk->log_console(msg.c_str());
                }
            }
        }

        static void log_prediction(const PredictionEvent& event)
        {
            if (!enabled_) return;

            stats_.total_predictions++;
            stats_.valid_predictions++; // Assuming valid if logged

            // Update computation time stats
            stats_.total_computation_time_ms += event.computation_time_ms;
            if (event.computation_time_ms > stats_.max_computation_time_ms)
                stats_.max_computation_time_ms = event.computation_time_ms;
            if (event.computation_time_ms < stats_.min_computation_time_ms)
                stats_.min_computation_time_ms = event.computation_time_ms;

            // Update hitchance distribution
            int hitchance_percent = static_cast<int>(event.hit_chance * 100.f);
            if (hitchance_percent < 20) stats_.hitchance_0_20++;
            else if (hitchance_percent < 40) stats_.hitchance_20_40++;
            else if (hitchance_percent < 60) stats_.hitchance_40_60++;
            else if (hitchance_percent < 80) stats_.hitchance_60_80++;
            else stats_.hitchance_80_100++;

            // Edge case counts
            if (event.was_dash) stats_.dash_predictions++;
            if (event.was_stationary) stats_.stationary_predictions++;
            if (event.was_animation_locked) stats_.animation_lock_predictions++;
            if (event.collision_detected) stats_.collision_detections++;

            if (event.edge_case == "stasis") stats_.stasis_predictions++;
            else if (event.edge_case == "channeling") stats_.channel_predictions++;

            // Per-spell-type stats
            stats_.spell_type_counts[event.spell_type]++;
            stats_.spell_type_avg_hitchance[event.spell_type] += event.hit_chance;

            // Per-target stats
            stats_.target_prediction_counts[event.target_name]++;
            stats_.target_avg_hitchance[event.target_name] += event.hit_chance;

            // Store event for detailed log
            events_.push_back(event);
        }

        static void log_invalid_prediction(const std::string& reason)
        {
            if (!enabled_) return;
            stats_.total_predictions++;
            stats_.invalid_predictions++;
        }

        static void log_pattern_detected(bool is_alternating)
        {
            if (!enabled_) return;
            stats_.patterns_detected++;
            if (is_alternating)
                stats_.alternating_patterns++;
            else
                stats_.repeating_patterns++;
        }

        static void finalize(float session_duration_seconds)
        {
            if (!enabled_) return;

            stats_.session_duration_seconds = session_duration_seconds;

            // Calculate averages for per-spell-type
            for (auto& pair : stats_.spell_type_avg_hitchance)
            {
                int count = stats_.spell_type_counts[pair.first];
                if (count > 0)
                    pair.second /= count;
            }

            // Calculate averages for per-target
            for (auto& pair : stats_.target_avg_hitchance)
            {
                int count = stats_.target_prediction_counts[pair.first];
                if (count > 0)
                    pair.second /= count;
            }

            // Write full report
            write_report();
        }

        static void write_report()
        {
            if (!enabled_) return;

            std::ofstream file(log_file_path_, std::ios::app);
            if (!file.is_open()) return;

            file << "\n\n";
            file << "=============================================================================\n";
            file << "SESSION SUMMARY\n";
            file << "=============================================================================\n\n";

            // Session info
            file << "Champion: " << stats_.champion_name << "\n";
            file << "Duration: " << stats_.session_duration_seconds << " seconds\n";
            file << "Total Predictions: " << stats_.total_predictions << "\n";
            file << "Valid: " << stats_.valid_predictions << " | Invalid: " << stats_.invalid_predictions << "\n\n";

            // Performance metrics
            file << "--- PERFORMANCE ---\n";
            float avg_time = stats_.total_computation_time_ms / std::max(1, stats_.valid_predictions);
            file << "Avg Computation Time: " << avg_time << " ms\n";
            file << "Min: " << stats_.min_computation_time_ms << " ms | Max: " << stats_.max_computation_time_ms << " ms\n";
            file << "Total CPU Time: " << stats_.total_computation_time_ms << " ms\n\n";

            // Hitchance distribution
            file << "--- HITCHANCE DISTRIBUTION ---\n";
            file << " 0-20%: " << stats_.hitchance_0_20 << " (" << (stats_.hitchance_0_20 * 100.f / std::max(1, stats_.valid_predictions)) << "%)\n";
            file << "20-40%: " << stats_.hitchance_20_40 << " (" << (stats_.hitchance_20_40 * 100.f / std::max(1, stats_.valid_predictions)) << "%)\n";
            file << "40-60%: " << stats_.hitchance_40_60 << " (" << (stats_.hitchance_40_60 * 100.f / std::max(1, stats_.valid_predictions)) << "%)\n";
            file << "60-80%: " << stats_.hitchance_60_80 << " (" << (stats_.hitchance_60_80 * 100.f / std::max(1, stats_.valid_predictions)) << "%)\n";
            file << "80-100%: " << stats_.hitchance_80_100 << " (" << (stats_.hitchance_80_100 * 100.f / std::max(1, stats_.valid_predictions)) << "%)\n\n";

            // Edge case stats
            file << "--- EDGE CASES ---\n";
            file << "Dash Predictions: " << stats_.dash_predictions << "\n";
            file << "Stasis Predictions: " << stats_.stasis_predictions << "\n";
            file << "Channel Predictions: " << stats_.channel_predictions << "\n";
            file << "Stationary Targets: " << stats_.stationary_predictions << "\n";
            file << "Animation Locked: " << stats_.animation_lock_predictions << "\n";
            file << "Collision Detected: " << stats_.collision_detections << "\n\n";

            // Pattern detection
            file << "--- PATTERN DETECTION ---\n";
            file << "Total Patterns: " << stats_.patterns_detected << "\n";
            file << "Alternating: " << stats_.alternating_patterns << " | Repeating: " << stats_.repeating_patterns << "\n\n";

            // Per-spell-type stats
            file << "--- PER SPELL TYPE ---\n";
            for (const auto& pair : stats_.spell_type_counts)
            {
                float avg_hc = stats_.spell_type_avg_hitchance[pair.first];
                file << pair.first << ": " << pair.second << " predictions, avg hitchance " << (avg_hc * 100.f) << "%\n";
            }
            file << "\n";

            // Per-target stats
            file << "--- PER TARGET ---\n";
            for (const auto& pair : stats_.target_prediction_counts)
            {
                float avg_hc = stats_.target_avg_hitchance[pair.first];
                file << pair.first << ": " << pair.second << " predictions, avg hitchance " << (avg_hc * 100.f) << "%\n";
            }
            file << "\n";

            // Detailed event log (last 100 events)
            file << "=============================================================================\n";
            file << "DETAILED EVENT LOG (Last 100 Events)\n";
            file << "=============================================================================\n\n";

            size_t start_idx = events_.size() > 100 ? events_.size() - 100 : 0;
            for (size_t i = start_idx; i < events_.size(); ++i)
            {
                const auto& e = events_[i];
                file << "[" << std::fixed << std::setprecision(2) << e.timestamp << "s] ";
                file << e.target_name << " | " << e.spell_type << " | ";
                file << "HC:" << static_cast<int>(e.hit_chance * 100) << "% | ";
                file << "Conf:" << static_cast<int>(e.confidence * 100) << "% | ";
                file << "Dist:" << static_cast<int>(e.distance) << " | ";
                file << e.edge_case;
                if (e.was_dash) file << " [DASH]";
                if (e.was_stationary) file << " [STILL]";
                if (e.was_animation_locked) file << " [LOCKED]";
                if (e.collision_detected) file << " [COLLISION]";
                file << " | " << std::fixed << std::setprecision(3) << e.computation_time_ms << "ms\n";
            }

            file << "\n=============================================================================\n";
            file << "END OF REPORT\n";
            file << "=============================================================================\n";

            file.close();

            // Log file path to console
            if (g_sdk)
            {
                char msg[512];
                snprintf(msg, sizeof(msg),
                    "[Danny.Prediction] Telemetry report saved: %d predictions logged to:\n%s",
                    stats_.total_predictions, log_file_path_.c_str());
                g_sdk->log_console(msg);
            }
        }
    };

    // Static member initialization
    inline SessionStats TelemetryLogger::stats_;
    inline std::vector<PredictionEvent> TelemetryLogger::events_;
    inline bool TelemetryLogger::enabled_ = false;
    inline std::string TelemetryLogger::log_file_path_;

} // namespace PredictionTelemetry
