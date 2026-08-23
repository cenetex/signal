#include "gameplay_observability.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#define OBS_HIST_BUCKETS 256u
#define OBS_HIST_BUCKET_MS 0.25
#define OBS_REPORT_CAP 8192u

typedef struct {
    uint64_t bucket[OBS_HIST_BUCKETS];
    uint64_t samples;
    double total_ms;
    double max_ms;
} obs_histogram_t;

typedef struct {
    uint64_t samples;
    double total_ms;
    double max_ms;
} obs_phase_stats_t;

typedef struct {
    uint64_t samples;
    float max_correction_world;
    float max_velocity_discontinuity;
    float max_correction_jerk;
} obs_entity_stats_t;

typedef struct {
    bool enabled;
    double report_interval_sec;
    double configured_at;
    double last_report_at;
    double frame_started_at;
    double frame_phase_ms[GAMEPLAY_PHASE_COUNT];
    obs_histogram_t frame;
    obs_histogram_t simulation;
    obs_phase_stats_t phase[GAMEPLAY_PHASE_COUNT];
    obs_entity_stats_t entity[GAMEPLAY_ENTITY_COUNT];
    uint64_t slow_16ms;
    uint64_t slow_33ms;
    uint64_t unexplained_slow_frames;
    uint64_t slow_cause[GAMEPLAY_PHASE_COUNT];
    uint64_t sim_steps;
    uint64_t missed_ticks;
    uint64_t accumulator_dropped_ticks;
    uint64_t packet_count;
    uint64_t packet_bytes;
    size_t max_packet_bytes;
    double max_packet_gap_ms;
    double last_packet_at[256];
    uint64_t packet_type_count[256];
    char report[OBS_REPORT_CAP];
} obs_state_t;

static obs_state_t obs;

static const char *const phase_names[GAMEPLAY_PHASE_COUNT] = {
    "input_network",
    "simulation",
    "interpolation",
    "world_render",
    "ui_render",
    "submission",
    "audio_media",
    "unattributed",
    "authority_sim",
    "loopback_codec",
};

static const char *const entity_names[GAMEPLAY_ENTITY_COUNT] = {
    "asteroid",
    "cargo_pod",
    "scaffold",
    "npc",
    "remote_player",
};

double gameplay_observability_now(void)
{
#ifdef __EMSCRIPTEN__
    return emscripten_get_now() / 1000.0;
#elif defined(_WIN32)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0)
        (void)QueryPerformanceFrequency(&frequency);
    (void)QueryPerformanceCounter(&counter);
    return frequency.QuadPart > 0
        ? (double)counter.QuadPart / (double)frequency.QuadPart : 0.0;
#else
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#endif
}

static void histogram_record(obs_histogram_t *histogram, double ms)
{
    if (!histogram || !isfinite(ms) || ms < 0.0) return;
    size_t bucket = (size_t)(ms / OBS_HIST_BUCKET_MS);
    if (bucket >= OBS_HIST_BUCKETS) bucket = OBS_HIST_BUCKETS - 1u;
    histogram->bucket[bucket]++;
    histogram->samples++;
    histogram->total_ms += ms;
    if (histogram->max_ms < ms) histogram->max_ms = ms;
}

static double histogram_percentile(const obs_histogram_t *histogram,
                                   double percentile)
{
    if (!histogram || histogram->samples == 0) return 0.0;
    uint64_t target = (uint64_t)ceil(
        percentile * (double)histogram->samples);
    if (target == 0) target = 1;
    uint64_t seen = 0;
    for (size_t i = 0; i < OBS_HIST_BUCKETS; i++) {
        seen += histogram->bucket[i];
        if (seen >= target)
            return ((double)i + 0.5) * OBS_HIST_BUCKET_MS;
    }
    return histogram->max_ms;
}

void gameplay_observability_configure(bool enabled,
                                      double report_interval_sec)
{
    memset(&obs, 0, sizeof(obs));
    obs.enabled = enabled;
    obs.report_interval_sec =
        isfinite(report_interval_sec) && report_interval_sec > 0.0
            ? report_interval_sec : 60.0;
    obs.configured_at = gameplay_observability_now();
    obs.last_report_at = obs.configured_at;
}

bool gameplay_observability_enabled(void)
{
    return obs.enabled;
}

void gameplay_observability_reset(void)
{
    bool enabled = obs.enabled;
    double interval = obs.report_interval_sec;
    gameplay_observability_configure(enabled, interval);
}

double gameplay_observability_phase_begin(void)
{
    return obs.enabled ? gameplay_observability_now() : 0.0;
}

void gameplay_observability_phase_end(
    gameplay_observability_phase_t phase, double started_at)
{
    if (!obs.enabled || phase < 0 || phase >= GAMEPLAY_PHASE_COUNT ||
        started_at <= 0.0) return;
    double elapsed_ms = (gameplay_observability_now() - started_at) * 1000.0;
    if (!isfinite(elapsed_ms) || elapsed_ms < 0.0) return;
    obs.frame_phase_ms[phase] += elapsed_ms;
    obs_phase_stats_t *stats = &obs.phase[phase];
    stats->samples++;
    stats->total_ms += elapsed_ms;
    if (stats->max_ms < elapsed_ms) stats->max_ms = elapsed_ms;
    if (phase == GAMEPLAY_PHASE_SIMULATION)
        histogram_record(&obs.simulation, elapsed_ms);
}

void gameplay_observability_frame_begin(void)
{
    if (!obs.enabled) return;
    memset(obs.frame_phase_ms, 0, sizeof(obs.frame_phase_ms));
    obs.frame_started_at = gameplay_observability_now();
}

static void report_append(size_t *offset, const char *format, ...)
{
    if (!offset || *offset >= sizeof(obs.report)) return;
    va_list args;
    va_start(args, format);
    int wrote = vsnprintf(obs.report + *offset,
                          sizeof(obs.report) - *offset,
                          format, args);
    va_end(args);
    if (wrote < 0) return;
    size_t available = sizeof(obs.report) - *offset;
    *offset += (size_t)wrote < available ? (size_t)wrote : available - 1u;
}

const char *gameplay_observability_report_json(void)
{
    size_t offset = 0;
    double now = gameplay_observability_now();
    double elapsed = now >= obs.configured_at
        ? now - obs.configured_at : 0.0;
    report_append(&offset,
        "{\"schema\":\"signal.gameplay-jank.v1\","
        "\"enabled\":%s,\"window_sec\":%.3f,"
        "\"frames\":%llu,\"frame_ms\":{"
        "\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f},"
        "\"simulation_ms\":{\"p50\":%.3f,\"p95\":%.3f,"
        "\"p99\":%.3f,\"max\":%.3f},"
        "\"slow_frames\":{\"over_16_6\":%llu,\"over_33_3\":%llu,"
        "\"unexplained\":%llu},"
        "\"fixed_step\":{\"completed\":%llu,\"missed\":%llu,"
        "\"accumulator_dropped\":%llu},"
        "\"snapshots\":{\"packets\":%llu,\"bytes\":%llu,"
        "\"max_packet_bytes\":%zu,\"max_gap_ms\":%.3f},"
        "\"phases\":{",
        obs.enabled ? "true" : "false", elapsed,
        (unsigned long long)obs.frame.samples,
        histogram_percentile(&obs.frame, 0.50),
        histogram_percentile(&obs.frame, 0.95),
        histogram_percentile(&obs.frame, 0.99), obs.frame.max_ms,
        histogram_percentile(&obs.simulation, 0.50),
        histogram_percentile(&obs.simulation, 0.95),
        histogram_percentile(&obs.simulation, 0.99), obs.simulation.max_ms,
        (unsigned long long)obs.slow_16ms,
        (unsigned long long)obs.slow_33ms,
        (unsigned long long)obs.unexplained_slow_frames,
        (unsigned long long)obs.sim_steps,
        (unsigned long long)obs.missed_ticks,
        (unsigned long long)obs.accumulator_dropped_ticks,
        (unsigned long long)obs.packet_count,
        (unsigned long long)obs.packet_bytes,
        obs.max_packet_bytes, obs.max_packet_gap_ms);

    for (int i = 0; i < GAMEPLAY_PHASE_COUNT; i++) {
        const obs_phase_stats_t *phase = &obs.phase[i];
        report_append(&offset,
            "%s\"%s\":{\"avg_ms\":%.3f,\"max_ms\":%.3f,"
            "\"slow_cause\":%llu}",
            i == 0 ? "" : ",", phase_names[i],
            phase->samples > 0
                ? phase->total_ms / (double)phase->samples : 0.0,
            phase->max_ms,
            (unsigned long long)obs.slow_cause[i]);
    }
    report_append(&offset, "},\"entities\":{");
    for (int i = 0; i < GAMEPLAY_ENTITY_COUNT; i++) {
        const obs_entity_stats_t *entity = &obs.entity[i];
        report_append(&offset,
            "%s\"%s\":{\"samples\":%llu,"
            "\"max_correction_world\":%.4f,"
            "\"max_velocity_discontinuity\":%.4f,"
            "\"max_correction_jerk\":%.4f}",
            i == 0 ? "" : ",", entity_names[i],
            (unsigned long long)entity->samples,
            entity->max_correction_world,
            entity->max_velocity_discontinuity,
            entity->max_correction_jerk);
    }
    report_append(&offset, "}}");
    return obs.report;
}

void gameplay_observability_frame_end(void)
{
    if (!obs.enabled || obs.frame_started_at <= 0.0) return;
    double now = gameplay_observability_now();
    double frame_ms = (now - obs.frame_started_at) * 1000.0;
    obs.frame_started_at = 0.0;
    if (!isfinite(frame_ms) || frame_ms < 0.0) return;
    histogram_record(&obs.frame, frame_ms);

    double attributed_before_remainder = 0.0;
    for (int i = 0; i < GAMEPLAY_PHASE_UNATTRIBUTED; i++)
        attributed_before_remainder += obs.frame_phase_ms[i];
    double remainder = fmax(0.0, frame_ms - attributed_before_remainder);
    obs.frame_phase_ms[GAMEPLAY_PHASE_UNATTRIBUTED] = remainder;
    obs_phase_stats_t *unattributed =
        &obs.phase[GAMEPLAY_PHASE_UNATTRIBUTED];
    unattributed->samples++;
    unattributed->total_ms += remainder;
    if (unattributed->max_ms < remainder)
        unattributed->max_ms = remainder;

    if (frame_ms > 16.6) {
        obs.slow_16ms++;
        double largest = 0.0;
        int cause = 0;
        for (int i = 0; i < GAMEPLAY_PRIMARY_PHASE_COUNT; i++) {
            if (obs.frame_phase_ms[i] > largest) {
                largest = obs.frame_phase_ms[i];
                cause = i;
            }
        }
        obs.slow_cause[cause]++;
    }
    if (frame_ms > 33.3) obs.slow_33ms++;

    if (obs.report_interval_sec > 0.0 &&
        now - obs.last_report_at >= obs.report_interval_sec) {
        obs.last_report_at = now;
        fprintf(stdout, "[gameplay-jank] %s\n",
                gameplay_observability_report_json());
    }
}

void gameplay_observability_record_sim_steps(
    uint32_t completed, uint32_t missed, uint32_t dropped)
{
    if (!obs.enabled) return;
    obs.sim_steps += completed;
    obs.missed_ticks += missed;
    obs.accumulator_dropped_ticks += dropped;
}

void gameplay_observability_record_packet(uint8_t type, size_t bytes)
{
    if (!obs.enabled) return;
    double now = gameplay_observability_now();
    double last = obs.last_packet_at[type];
    if (last > 0.0 && now >= last) {
        double gap_ms = (now - last) * 1000.0;
        if (obs.max_packet_gap_ms < gap_ms)
            obs.max_packet_gap_ms = gap_ms;
    }
    obs.last_packet_at[type] = now;
    obs.packet_type_count[type]++;
    obs.packet_count++;
    obs.packet_bytes += bytes;
    if (obs.max_packet_bytes < bytes) obs.max_packet_bytes = bytes;
}

void gameplay_observability_record_entity_correction(
    gameplay_observability_entity_t entity,
    float correction_world, float velocity_discontinuity,
    float packet_gap_sec)
{
    if (!obs.enabled || entity < 0 || entity >= GAMEPLAY_ENTITY_COUNT ||
        !isfinite(correction_world) || correction_world < 0.0f ||
        !isfinite(velocity_discontinuity) ||
        velocity_discontinuity < 0.0f) return;
    obs_entity_stats_t *stats = &obs.entity[entity];
    stats->samples++;
    if (stats->max_correction_world < correction_world)
        stats->max_correction_world = correction_world;
    if (stats->max_velocity_discontinuity < velocity_discontinuity)
        stats->max_velocity_discontinuity = velocity_discontinuity;
    if (isfinite(packet_gap_sec) && packet_gap_sec > 0.0001f) {
        float jerk = correction_world /
            (packet_gap_sec * packet_gap_sec);
        if (isfinite(jerk) && stats->max_correction_jerk < jerk)
            stats->max_correction_jerk = jerk;
    }
}
