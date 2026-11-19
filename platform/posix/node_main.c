/* SPDX-License-Identifier: MIT */
/*
 * Host entry point of the node: parses the command line, brings up the POSIX platform layer
 * and the simulated hardware, starts the application and, after an orderly stop, writes the
 * run's statistics as JSON.
 *
 * Everything here runs on the main thread before the scheduler starts or after it has ended,
 * which is why stdio streams are used freely; tasks never write to one.
 */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app.h"
#include "app_tasks.h"
#include "can_backend.h"
#include "plat_can.h"
#include "posix_platform.h"
#include "posix_sensor.h"
#include "posix_sim.h"

#ifndef FCN_VERSION
#define FCN_VERSION "unknown"
#endif

/* Whether the build has a sanitizer that adds to every stack frame (AddressSanitizer's
 * redzones, ThreadSanitizer's calls into its runtime). The stack figures of such a build are
 * not those of the code, and the statistics say so ("stack_instrumented"). */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define STACK_INSTRUMENTED "true"
#else
#define STACK_INSTRUMENTED "false"
#endif

#define DEFAULT_CAN_SPEC "udp"
#define DEFAULT_BITRATE  500000U
#define MAX_BITRATE      PLAT_CAN_MAX_BITRATE

typedef struct {
    posix_config_t platform;
    app_config_t app;
    const char *stats_path;
    const char *can_spec;
    uint32_t bitrate;
    uint32_t sensor_seed;
    uint16_t sim_port;
} node_options_t;

static void print_usage(const char *program)
{
    (void)printf("usage: %s [options]\n"
                 "  --can SPEC             CAN transport: udp[:<base port>] (default, port %u) or\n"
                 "                         socketcan:<interface>, for example socketcan:vcan0\n"
                 "  --bitrate N            bit rate of the modelled bus in bit/s (default %u);\n"
                 "                         0 switches the bus model's pacing off\n"
                 "  --sim-port N           serve simulation control (fault injection) on UDP\n"
                 "                         port N of the loopback interface (default: off)\n"
                 "  --sensor-seed N        seed of the sensor model's noise, 0 to 4294967295\n"
                 "                         (default %u); the same seed gives the same readings\n"
                 "  --duration-s N         stop after N seconds of tick time (default and 0: run\n"
                 "                         until SIGINT or SIGTERM)\n"
                 "  --stats-json PATH      write run statistics to PATH on an orderly stop\n"
                 "  --tick-mode MODE       locked (default) or stock\n"
                 "  --irq-mode MODE        signal (default) or poll\n"
                 "  --log-file PATH        write the log to PATH instead of stderr\n"
                 "  --log-level LEVEL      error, warn, info (default) or debug\n"
                 "  --version              print the version and exit\n"
                 "  --help                 print this text and exit\n"
                 "exit status: 0 after an orderly stop; 1 on a usage or start-up error; %d when\n"
                 "the CAN transport went away while the node ran (the statistics are written);\n"
                 "%d when an orderly stop hung; 10 to 22 for a reset of the node\n"
                 "(platform/include/platform.h)\n",
                 program, (unsigned int)CAN_UDP_DEFAULT_PORT, (unsigned int)DEFAULT_BITRATE,
                 (unsigned int)POSIX_SENSOR_DEFAULT_SEED, POSIX_EXIT_CAN_TRANSPORT_LOST,
                 POSIX_EXIT_SHUTDOWN_TIMEOUT);
}

static bool parse_log_level(const char *text, log_level_t *out)
{
    static const struct {
        const char *name;
        log_level_t level;
    } table[] = {{"error", LOG_LEVEL_ERROR},
                 {"warn", LOG_LEVEL_WARN},
                 {"info", LOG_LEVEL_INFO},
                 {"debug", LOG_LEVEL_DEBUG}};
    bool found = false;

    for (size_t i = 0U; i < (sizeof(table) / sizeof(table[0])); i++) {
        if (strcmp(text, table[i].name) == 0) {
            *out = table[i].level;
            found = true;
        }
    }

    return found;
}

/* A decimal number of at most `max`, with nothing before or after it. */
static bool parse_number(const char *text, unsigned long max, unsigned long *out)
{
    char *end = NULL;
    bool ok = false;

    if ((text[0] >= '0') && (text[0] <= '9')) {
        const unsigned long value = strtoul(text, &end, 10);

        if ((*end == '\0') && (value <= max)) {
            *out = value;
            ok = true;
        }
    }

    return ok;
}

/* Returns 0 to continue, 1 to exit successfully (--help, --version), 2 on a usage error. */
static int parse_options(int argc, char **argv, node_options_t *options)
{
    enum {
        OPT_DURATION = 1,
        OPT_STATS,
        OPT_TICK,
        OPT_IRQ,
        OPT_LOG_FILE,
        OPT_LOG_LEVEL,
        OPT_CAN,
        OPT_BITRATE,
        OPT_SIM_PORT,
        OPT_SENSOR_SEED,
        OPT_VERSION,
        OPT_HELP
    };
    static const struct option long_options[] = {
        {"duration-s", required_argument, NULL, OPT_DURATION},
        {"stats-json", required_argument, NULL, OPT_STATS},
        {"tick-mode", required_argument, NULL, OPT_TICK},
        {"irq-mode", required_argument, NULL, OPT_IRQ},
        {"log-file", required_argument, NULL, OPT_LOG_FILE},
        {"log-level", required_argument, NULL, OPT_LOG_LEVEL},
        {"can", required_argument, NULL, OPT_CAN},
        {"bitrate", required_argument, NULL, OPT_BITRATE},
        {"sim-port", required_argument, NULL, OPT_SIM_PORT},
        {"sensor-seed", required_argument, NULL, OPT_SENSOR_SEED},
        {"version", no_argument, NULL, OPT_VERSION},
        {"help", no_argument, NULL, OPT_HELP},
        {NULL, 0, NULL, 0}};
    int result = 0;
    int option;

    while ((result == 0) && ((option = getopt_long(argc, argv, "", long_options, NULL)) != -1)) {
        unsigned long number = 0UL;

        switch (option) {
        case OPT_DURATION:
            if (parse_number(optarg, 86400UL, &number)) {
                options->app.run_seconds = (uint32_t)number;
            } else {
                (void)fprintf(stderr, "invalid --duration-s: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_STATS:
            options->stats_path = optarg;
            break;
        case OPT_TICK:
            if (strcmp(optarg, "locked") == 0) {
                options->platform.tick_mode = POSIX_TICK_LOCKED;
            } else if (strcmp(optarg, "stock") == 0) {
                options->platform.tick_mode = POSIX_TICK_STOCK;
            } else {
                (void)fprintf(stderr, "invalid --tick-mode: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_IRQ:
            if (strcmp(optarg, "signal") == 0) {
                options->platform.irq_mode = POSIX_IRQ_SIGNAL;
            } else if (strcmp(optarg, "poll") == 0) {
                options->platform.irq_mode = POSIX_IRQ_POLL;
            } else {
                (void)fprintf(stderr, "invalid --irq-mode: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_LOG_FILE:
            options->platform.log_path = optarg;
            break;
        case OPT_LOG_LEVEL:
            if (!parse_log_level(optarg, &options->platform.log_level)) {
                (void)fprintf(stderr, "invalid --log-level: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_CAN:
            options->can_spec = optarg;
            break;
        case OPT_BITRATE:
            if (parse_number(optarg, MAX_BITRATE, &number)) {
                options->bitrate = (uint32_t)number;
            } else {
                (void)fprintf(stderr, "invalid --bitrate: %s (0 to %u)\n", optarg,
                              (unsigned int)MAX_BITRATE);
                result = 2;
            }
            break;
        case OPT_SIM_PORT:
            if (parse_number(optarg, 65535UL, &number)) {
                options->sim_port = (uint16_t)number;
            } else {
                (void)fprintf(stderr, "invalid --sim-port: %s\n", optarg);
                result = 2;
            }
            break;
        case OPT_SENSOR_SEED:
            if (parse_number(optarg, 4294967295UL, &number)) {
                options->sensor_seed = (uint32_t)number;
            } else {
                (void)fprintf(stderr, "invalid --sensor-seed: %s (0 to 4294967295)\n", optarg);
                result = 2;
            }
            break;
        case OPT_VERSION:
            (void)printf("fcn_node %s (FreeRTOS %s)\n", FCN_VERSION, tskKERNEL_VERSION_NUMBER);
            result = 1;
            break;
        case OPT_HELP:
            print_usage(argv[0]);
            result = 1;
            break;
        default:
            print_usage(argv[0]);
            result = 2;
            break;
        }
    }

    if ((result == 0) && (optind < argc)) {
        (void)fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
        result = 2;
    }

    return result;
}

/* One "key": value member of a JSON object; `separator` is what follows it. */
static void put_u64(FILE *file, const char *key, uint64_t value, const char *separator)
{
    (void)fprintf(file, "\"%s\": %llu%s", key, (unsigned long long)value, separator);
}

static void put_i64(FILE *file, const char *key, int64_t value, const char *separator)
{
    (void)fprintf(file, "\"%s\": %lld%s", key, (long long)value, separator);
}

static void put_bool(FILE *file, const char *key, bool value, const char *separator)
{
    (void)fprintf(file, "\"%s\": %s%s", key, value ? "true" : "false", separator);
}

static const char *can_state_name(uint8_t state)
{
    static const char *const names[] = {"active", "passive", "busoff"};

    return (state <= PLAT_CAN_BUS_OFF) ? names[state] : "unknown";
}

/*
 * The "can" member. The layers that report here are sampled at two moments:
 *
 *  - by the health task, just before it ended the scheduler: "queue", and in "driver" the
 *    counters of the CAN tasks (rx_frames, rx_unhandled, rx_batches_cut, tx_load_failed);
 *  - after the hardware thread has stopped: "controller", "transport_counters", "pacing",
 *    and in "driver" the counters of the controller's FIFOs (rx_fifo_overruns,
 *    rx_fifo_pending, event_fifo_overruns, state_events_merged).
 *
 * In between, no task ran, but the hardware thread did. A frame transmitted then is counted
 * by the controller and not by the queue; a frame received then went into the receive FIFO
 * (rx_fifo_pending) or, with the FIFO full, was lost there (rx_fifo_overruns). So for the
 * receive path the counts agree exactly:
 *     controller.rx_frames == driver.rx_frames + driver.rx_fifo_overruns
 *                             + driver.rx_fifo_pending
 */
static void write_can_stats(FILE *file, const node_stats_t *stats)
{
    posix_can_report_t report;
    posix_can_stats_t host;

    posix_can_get_report(&report);
    posix_can_get_stats(&host);

    (void)fprintf(file, "  \"can\": {\n    \"transport\": \"%s\", \"bitrate\": %u, ",
                  report.transport, (unsigned int)report.bitrate);
    put_u64(file, "elapsed_ns", report.elapsed_ns, ",\n");
    (void)fprintf(file, "    \"state\": \"%s\", \"tec\": %u, \"rec\": %u,\n",
                  can_state_name(report.state), (unsigned int)report.tec, (unsigned int)report.rec);

    /* Why the transport ended the run, or null. The message is strerror()'s, which contains
     * no character that would need escaping in a JSON string. */
    if (report.transport_error != 0) {
        (void)fprintf(file, "    \"transport_error\": \"%s\",\n", strerror(report.transport_error));
    } else {
        (void)fprintf(file, "    \"transport_error\": null,\n");
    }

    (void)fprintf(file, "    \"controller\": {");
    put_u64(file, "tx_frames", report.tx_frames, ", ");
    put_u64(file, "tx_attempts_failed", report.tx_attempts_failed, ", ");
    put_u64(file, "tx_aborted", report.tx_aborted, ", ");
    put_u64(file, "rx_frames", report.rx_frames, ", ");
    put_u64(file, "rx_errors", report.rx_errors, ", ");
    put_u64(file, "rx_lost_fault", report.rx_lost_fault, ", ");
    put_u64(file, "wire_overruns", report.wire_overruns, ", ");
    put_u64(file, "bus_off_events", report.bus_off_events, ", ");
    put_u64(file, "late_resyncs", report.late_resyncs, ", ");
    put_u64(file, "busy_ns", report.busy_ns, "},\n");

    (void)fprintf(file, "    \"transport_counters\": {");
    put_u64(file, "tx_frames", report.transport_tx_frames, ", ");
    put_u64(file, "tx_failed", report.transport_tx_failed, ", ");
    put_u64(file, "rx_frames", report.transport_rx_frames, ", ");
    put_u64(file, "rx_malformed", report.transport_rx_malformed, ", ");
    put_u64(file, "rx_overflow", report.transport_rx_overflow, ", ");
    put_u64(file, "rx_failed", report.transport_rx_failed, "},\n");

    /* How late the host carried out the model's times; see posix_can_stats_t. */
    (void)fprintf(file, "    \"pacing\": {");
    put_u64(file, "frames", host.frames_paced, ", ");
    put_u64(file, "lag_max_ns", host.pacing_lag_max_ns, ", ");
    put_u64(file, "lag_mean_ns", host.pacing_lag_mean_ns, ", ");
    put_u64(file, "wakeups", host.wakeups, ", ");
    put_u64(file, "wakeup_lag_max_ns", host.wakeup_lag_max_ns, ", ");
    put_u64(file, "wakeup_lag_mean_ns", host.wakeup_lag_mean_ns, ", ");
    put_u64(file, "rx_frames", host.rx_delivered, ", ");
    put_u64(file, "rx_lag_max_ns", host.rx_lag_max_ns, ", ");
    put_u64(file, "rx_lag_mean_ns", host.rx_lag_mean_ns, "},\n");

    (void)fprintf(file, "    \"driver\": {");
    put_u64(file, "rx_frames", stats->can_io.rx_frames, ", ");
    put_u64(file, "rx_unhandled", stats->can_io.rx_unhandled, ", ");
    put_u64(file, "rx_batches_cut", stats->can_io.rx_batches_cut, ", ");
    put_u64(file, "tx_load_failed", stats->can_io.tx_load_failed, ", ");
    put_u64(file, "rx_cycles", stats->can_io.rx_cycles, ", ");
    put_u64(file, "rx_cycles_skipped", stats->can_io.rx_cycles_skipped, ", ");
    put_u64(file, "rx_fifo_overruns", host.rx_fifo_overruns, ", ");
    put_u64(file, "event_fifo_overruns", host.event_fifo_overruns, ", ");
    put_u64(file, "state_events_merged", host.state_events_merged, ", ");
    put_u64(file, "rx_fifo_pending", host.rx_fifo_pending, "},\n");

    (void)fprintf(file, "    \"queue\": {");
    put_u64(file, "produced", stats->can_queue.produced, ", ");
    put_u64(file, "sent", stats->can_queue.sent, ", ");
    put_u64(file, "superseded", stats->can_queue.superseded, ", ");
    put_u64(file, "dropped", stats->can_queue.dropped, ", ");
    put_u64(file, "expired", stats->can_queue.expired, ", ");
    put_u64(file, "pending", stats->can_queue.pending, ", ");
    put_u64(file, "bus_off_count", stats->can_queue.bus_off_count, ", ");
    put_u64(file, "restart_requests", stats->can_queue.restart_requests, ", ");
    put_u64(file, "recall_requests", stats->can_queue.recall_requests, ", ");
    put_u64(file, "holdoff_ms", stats->can_queue.holdoff_ms, "}\n  },\n");
}

/*
 * "task_table": the task table of the node (app/task_table.c) in the input schema of the
 * schedulability check, so that `python -m fcn_tools.sched` can read the member as it is. The
 * strings of the table contain no character that would need escaping.
 */
static void write_task_table(FILE *file)
{
    size_t count = 0U;
    const app_task_budget_t *table = app_task_table(&count);

    (void)fprintf(file,
                  "  \"task_table\": {\n    \"schema\": \"fcn.task_table/1\",\n"
                  "    \"wcet_basis\": \"design\",\n    \"description\": \"%s\",\n"
                  "    \"tasks\": [",
                  app_task_table_description());

    for (size_t i = 0U; i < count; i++) {
        const app_task_budget_t *task = &table[i];

        (void)fprintf(file, "%s\n      {\"name\": \"%s\", \"priority\": %u, \"kind\": \"%s\", ",
                      (i == 0U) ? "" : ",", task->name, (unsigned int)task->priority,
                      task->periodic ? "periodic" : "sporadic");
        put_u64(file, task->periodic ? "period_us" : "min_interarrival_us", task->period_us, ", ");
        put_u64(file, "deadline_us", task->deadline_us, ", ");
        put_u64(file, "wcet_us", task->wcet_us, ", ");
        put_u64(file, "jitter_us", task->jitter_us, ", ");
        put_u64(file, "nonpreemptive_us", task->nonpreemptive_us, ", ");
        (void)fprintf(file, "\"critical_sections\": [");

        if (task->mutex != NULL) {
            (void)fprintf(file, "{\"resource\": \"%s\", \"length_us\": %u}", task->mutex,
                          (unsigned int)task->mutex_us);
        }

        (void)fprintf(file, "]}");
    }

    (void)fprintf(file, "\n    ]\n  },\n");
}

static const char *pack_state_name(bms_state_t state)
{
    static const char *const names[] = {"STARTUP", "NORMAL", "FAULT"};

    return ((uint32_t)state <= (uint32_t)BMS_STATE_FAULT) ? names[state] : "unknown";
}

static const char *sm_state_name(e2e_sm_state_t state)
{
    static const char *const names[] = {"DEINIT", "NODATA", "INIT", "VALID", "INVALID"};

    return ((uint32_t)state <= (uint32_t)E2E_SM_INVALID) ? names[state] : "unknown";
}

/* "sensor", "sample_queue", "process" and "pack": the measurement chain, as the health task
 * captured it when it ended the run. */
static void write_measurement_stats(FILE *file, const node_stats_t *stats)
{
    const node_sensor_stats_t *sensor = &stats->sensor;
    const node_process_stats_t *process = &stats->process;
    const bms_pack_t *pack = &stats->pack;

    (void)fprintf(file, "  \"sensor\": {");
    put_u64(file, "periods", sensor->periods, ", ");
    put_u64(file, "overruns", sensor->overruns, ", ");
    (void)fprintf(file, "\"reads\": {");
    put_u64(file, "current", sensor->current_reads, ", ");
    put_u64(file, "cells", sensor->cell_reads, ", ");
    put_u64(file, "temps", sensor->temp_reads, "}, ");
    (void)fprintf(file, "\"unanswered\": {");
    put_u64(file, "current", sensor->current_unanswered, ", ");
    put_u64(file, "cells", sensor->cell_unanswered, ", ");
    put_u64(file, "temps", sensor->temp_unanswered, "}},\n");

    (void)fprintf(file, "  \"sample_queue\": {");
    put_u64(file, "length", APP_SAMPLE_QUEUE_LENGTH, ", ");
    put_u64(file, "posted", sensor->samples_posted, ", ");
    put_u64(file, "full", sensor->queue_full, ", ");
    put_u64(file, "high_water", process->high_water, "},\n");

    (void)fprintf(file, "  \"process\": {");
    put_u64(file, "samples", process->samples, ", ");
    put_u64(file, "timeouts", process->timeouts, ", ");
    put_u64(file, "backlog", process->backlog, "},\n");

    (void)fprintf(file, "  \"pack\": {\"state\": \"%s\", ", pack_state_name(pack->state));
    put_u64(file, "updates", pack->updates, ", ");
    put_i64(file, "current_ma", pack->current_ma, ", ");
    put_bool(file, "current_valid", pack->current_valid, ", ");
    put_u64(file, "pack_mv", pack->pack_mv, ", ");
    put_bool(file, "pack_valid", pack->pack_valid, ",\n    ");
    (void)fprintf(file, "\"cell_mv\": [");
    for (uint32_t i = 0U; i < BMS_CELLS; i++) {
        (void)fprintf(file, "%s%u", (i == 0U) ? "" : ", ", (unsigned int)pack->cell_mv[i]);
    }
    (void)fprintf(file, "], ");
    put_u64(file, "cell_valid", pack->cell_valid, ", ");
    (void)fprintf(file, "\"temp_dc\": [");
    for (uint32_t i = 0U; i < BMS_TEMPS; i++) {
        (void)fprintf(file, "%s%d", (i == 0U) ? "" : ", ", (int)pack->temp_dc[i]);
    }
    (void)fprintf(file, "], ");
    put_u64(file, "temp_valid", pack->temp_valid, ",\n    ");
    put_u64(file, "min_cell_mv", pack->min_cell_mv, ", ");
    put_u64(file, "min_cell", pack->min_cell, ", ");
    put_u64(file, "max_cell_mv", pack->max_cell_mv, ", ");
    put_u64(file, "max_cell", pack->max_cell, ",\n    ");
    put_u64(file, "flagged_readings", pack->flagged_readings, ", ");
    put_u64(file, "implausible_readings", pack->implausible_readings, ", ");
    put_u64(file, "lost_samples", pack->lost_samples, ", ");
    put_u64(file, "sequence_errors", pack->sequence_errors, "},\n");
}

/* "telemetry": the switch, and per cyclic message what the application did with it and what
 * its slot of the transmit queue did (the queue's counters were captured with "can.queue"). */
static void write_telemetry_stats(FILE *file, const node_stats_t *stats)
{
    (void)fprintf(file, "  \"telemetry\": {");
    put_bool(file, "enabled", stats->telemetry_on, ", ");
    (void)fprintf(file, "\"messages\": {");

    for (uint32_t i = 0U; i < NODE_STATS_MESSAGES; i++) {
        const node_message_stats_t *message = &stats->messages[i];

        (void)fprintf(file, "%s\n    \"%s\": {", (i == 0U) ? "" : ",",
                      (message->name != NULL) ? message->name : "unknown");
        put_u64(file, "id", message->id, ", ");
        put_u64(file, "slot", message->slot, ", ");
        put_u64(file, "cycle_ms", message->cycle_ms, ", ");

        if (message->protected_e2e) {
            put_u64(file, "e2e_data_id", message->data_id, ", ");
        } else {
            (void)fprintf(file, "\"e2e_data_id\": null, ");
        }

        put_bool(file, "gated", message->gated, ", ");
        put_u64(file, "published", message->published, ", ");
        put_u64(file, "suppressed", message->suppressed, ", ");
        put_u64(file, "failed", message->failed, ",\n      ");
        (void)fprintf(file, "\"queue\": {");
        put_u64(file, "produced", message->queue.produced, ", ");
        put_u64(file, "sent", message->queue.sent, ", ");
        put_u64(file, "superseded", message->queue.superseded, ", ");
        put_u64(file, "pending", message->queue.pending, "}}");
    }

    (void)fprintf(file, "\n  }},\n");
}

/* "command": the receiver of TESTER_Command (app/command.h). */
static void write_command_stats(FILE *file, const node_stats_t *stats)
{
    const command_rx_t *command = &stats->command;

    (void)fprintf(file, "  \"command\": {");
    put_u64(file, "frames", command->frames, ", ");
    put_u64(file, "cycles", command->cycles, ", ");
    put_u64(file, "checks", command->checks, ", ");
    put_u64(file, "check_errors", command->check_errors, ", ");
    put_u64(file, "sm_errors", command->sm_errors, ",\n    ");
    (void)fprintf(file, "\"status\": {");
    put_u64(file, "OK", command->status.ok, ", ");
    put_u64(file, "OKSOMELOST", command->status.ok_some_lost, ", ");
    put_u64(file, "REPEATED", command->status.repeated, ", ");
    put_u64(file, "WRONGSEQUENCE", command->status.wrong_sequence, ", ");
    put_u64(file, "ERROR", command->status.error, ", ");
    put_u64(file, "NONEWDATA", command->status.no_new_data, "},\n    ");
    (void)fprintf(file, "\"sm_state\": \"%s\", ", sm_state_name(command->sm.sm_state));
    put_u64(file, "sm_ok_count", command->sm.ok_count, ", ");
    put_u64(file, "sm_error_count", command->sm.error_count, ", ");
    put_u64(file, "accepted", command->accepted, ", ");
    put_u64(file, "ignored", command->ignored, ", ");
    put_bool(file, "telemetry_on", command->telemetry_on, ", ");

    if (command->have_command) {
        put_u64(file, "clear_faults_seq", command->clear_faults_seq, "}\n");
    } else {
        (void)fprintf(file, "\"clear_faults_seq\": null}\n");
    }
}

static bool write_stats(const char *path, const node_options_t *options, uint64_t elapsed_ns)
{
    const node_stats_t *stats = app_final_stats();
    plat_stack_info_t stacks[NODE_STATS_MAX_TASKS];
    const size_t stack_count = plat_stack_report(stacks, NODE_STATS_MAX_TASKS);
    posix_tick_stats_t tick;
    posix_irq_stats_t irq;
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        return false;
    }

    posix_tick_get_stats(&tick);
    posix_irq_get_stats(&irq);

    /* Within one schema number, members are only ever added. */
    (void)fprintf(file, "{\n  \"schema\": \"fcn.node_stats/1\",\n");
    (void)fprintf(file, "  \"version\": \"%s\",\n", FCN_VERSION);
    (void)fprintf(file, "  \"kernel\": \"%s\",\n", tskKERNEL_VERSION_NUMBER);
    (void)fprintf(file, "  \"stack_instrumented\": %s,\n", STACK_INSTRUMENTED);
    (void)fprintf(file, "  \"elapsed_ns\": %llu,\n", (unsigned long long)elapsed_ns);
    (void)fprintf(file,
                  "  \"tick\": {\"mode\": \"%s\", \"rate_hz\": %u, \"count\": %llu, "
                  "\"thread_seen\": %s, \"signals_sent\": %llu, \"catch_ups\": %llu, "
                  "\"dropped\": %llu},\n",
                  /* The mode the tick code ran in, not the option: a test can then tell
                   * whether the option took effect. */
                  (tick.mode == POSIX_TICK_LOCKED) ? "locked" : "stock",
                  (unsigned int)configTICK_RATE_HZ, (unsigned long long)stats->tick_count,
                  tick.tick_thread_seen ? "true" : "false", (unsigned long long)tick.signals_sent,
                  (unsigned long long)tick.catch_ups, (unsigned long long)tick.dropped_ticks);
    (void)fprintf(file,
                  "  \"irq\": {\"mode\": \"%s\", \"raised\": %llu, \"signals_sent\": %llu, "
                  "\"serviced_by_signal\": %llu, \"serviced_by_tick\": %llu, "
                  "\"max_nesting\": %u},\n",
                  (options->platform.irq_mode == POSIX_IRQ_SIGNAL) ? "signal" : "poll",
                  (unsigned long long)irq.raised, (unsigned long long)irq.signals_sent,
                  (unsigned long long)irq.serviced_by_signal,
                  (unsigned long long)irq.serviced_by_tick, (unsigned int)irq.max_nesting);
    (void)fprintf(file, "  \"log_lines_dropped\": %u,\n", (unsigned int)posix_log_dropped());
    write_can_stats(file, stats);
    (void)fprintf(file, "  \"run_time_total_us\": %llu,\n",
                  (unsigned long long)stats->total_run_time_us);
    (void)fprintf(file, "  \"tasks\": [");

    for (uint32_t i = 0U; i < stats->task_count; i++) {
        const node_task_stats_t *task = &stats->tasks[i];
        /* -1: the task never registered with plat_task_enter(), so nothing is known. */
        long long stack_peak = -1;
        long long stack_start = -1;
        long long stack_size = -1;

        for (size_t k = 0U; k < stack_count; k++) {
            if (strcmp(stacks[k].name, task->name) == 0) {
                stack_peak = (long long)stacks[k].peak_bytes;
                stack_start = (long long)stacks[k].start_bytes;
                stack_size = (long long)stacks[k].configured_bytes;
            }
        }

        /* See plat_stack_info_t: stack_peak_bytes is the depth used of the host stack,
         * stack_start_bytes the part of it in use before the task's code ran, and
         * stack_size_bytes the stack the task was created with, the size meant for the
         * target. */
        (void)fprintf(file,
                      "%s\n    {\"name\": \"%s\", \"priority\": %u, \"run_time_us\": %llu, "
                      "\"stack_peak_bytes\": %lld, \"stack_start_bytes\": %lld, "
                      "\"stack_size_bytes\": %lld, \"overruns\": %u}",
                      (i == 0U) ? "" : ",", task->name, (unsigned int)task->priority,
                      (unsigned long long)task->run_time_us, stack_peak, stack_start, stack_size,
                      (unsigned int)task->overruns);
    }

    (void)fprintf(file, "\n  ],\n");
    write_task_table(file);
    write_measurement_stats(file, stats);
    write_telemetry_stats(file, stats);
    write_command_stats(file, stats);
    (void)fprintf(file, "}\n");
    return (fclose(file) == 0);
}

/* True if the node will be able to create or overwrite the file at `path` (NULL: no file).
 * Checked before the run, so that a mistyped path is a start-up error and not a log that
 * quietly goes elsewhere or statistics that are lost after an hour's run. The file itself is
 * not created here: a node that dies before it writes its statistics leaves none. */
static bool output_path_usable(const char *path, const char *option, char *error, size_t error_len)
{
    struct stat info;
    bool usable = true;

    if (path == NULL) {
        return true;
    }

    if (stat(path, &info) == 0) {
        if (S_ISDIR(info.st_mode)) {
            errno = EISDIR;
            usable = false;
        } else {
            usable = (access(path, W_OK) == 0);
        }
    } else if (errno == ENOENT) {
        /* The file does not exist yet: its directory has to, and it has to be writable. */
        char directory[4096];
        const char *slash = strrchr(path, '/');
        const size_t length = (slash == NULL) ? 0U : (size_t)(slash - path);

        if (slash == NULL) {
            usable = (access(".", W_OK | X_OK) == 0);
        } else if (length == 0U) {
            usable = (access("/", W_OK | X_OK) == 0);
        } else if (length >= sizeof(directory)) {
            errno = ENAMETOOLONG;
            usable = false;
        } else {
            (void)memcpy(directory, path, length);
            directory[length] = '\0';
            usable = (access(directory, W_OK | X_OK) == 0);
        }
    } else {
        usable = false;
    }

    if (!usable) {
        (void)snprintf(error, error_len, "cannot write %s %s: %s", option, path, strerror(errno));
    }

    return usable;
}

/* What can be wrong with a configuration whose options each parsed: checked before anything
 * is started. */
static bool check_configuration(const node_options_t *options, char *error, size_t error_len)
{
    can_backend_spec_t spec;

    if (!can_backend_parse_spec(options->can_spec, &spec, error, error_len)) {
        return false;
    }

    /* The simulation control socket must not be one of the bus's ports. It would take a slot
     * of the bus, receive every frame sent on it, and answer each with an error datagram. */
    if ((spec.kind == CAN_BACKEND_UDP) && (options->sim_port != 0U) &&
        (options->sim_port >= spec.udp_base_port) &&
        ((uint32_t)options->sim_port < ((uint32_t)spec.udp_base_port + CAN_UDP_SLOTS))) {
        (void)snprintf(
            error, error_len, "--sim-port %u is one of the ports of the UDP bus %s (%u to %u)",
            (unsigned int)options->sim_port, options->can_spec, (unsigned int)spec.udp_base_port,
            (unsigned int)spec.udp_base_port + CAN_UDP_SLOTS - 1U);
        return false;
    }

    return output_path_usable(options->platform.log_path, "--log-file", error, error_len) &&
           output_path_usable(options->stats_path, "--stats-json", error, error_len);
}

int main(int argc, char **argv)
{
    node_options_t options;
    char error[1024] = {0}; /* room for a message that quotes a long path */
    int status;

    (void)memset(&options, 0, sizeof(options));
    options.platform.tick_mode = POSIX_TICK_LOCKED;
    options.platform.irq_mode = POSIX_IRQ_SIGNAL;
    options.platform.log_level = LOG_LEVEL_INFO;
    options.can_spec = DEFAULT_CAN_SPEC;
    options.bitrate = DEFAULT_BITRATE;
    options.sensor_seed = POSIX_SENSOR_DEFAULT_SEED;

    status = parse_options(argc, argv, &options);

    if (status != 0) {
        return (status == 1) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* A test harness passes its process id here so that no node is left behind when the
     * harness dies: the kernel is asked to kill the node when its parent exits. The request
     * only covers a parent that is still alive when it is made, hence the check after it: if
     * the harness died before this point, the node already has another parent and ends here. */
    const char *const harness = getenv("FCN_DIE_WITH_PARENT");

    if (harness != NULL) {
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL);

        if ((long)getppid() != strtol(harness, NULL, 10)) {
            return EXIT_FAILURE;
        }
    }

    if (!check_configuration(&options, error, sizeof(error))) {
        (void)fprintf(stderr, "fcn_node: %s\n", error);
        return EXIT_FAILURE;
    }

    posix_platform_init(&options.platform);
    LOG_INF("main", "fcn_node %s starting (FreeRTOS %s)", FCN_VERSION, tskKERNEL_VERSION_NUMBER);

    /* The simulated hardware comes up before the application, as a board is powered before
     * its firmware runs. Without a bus there is no node: failing here is a start-up error. The
     * sensor model starts before the CAN hardware thread, which serves its simulation control
     * commands and may only start once every model has registered them. */
    bool hardware_up = posix_sim_open(options.sim_port, error, sizeof(error));

    if (hardware_up) {
        posix_sensor_start(options.sensor_seed);
        hardware_up = posix_can_start(options.can_spec, options.bitrate, error, sizeof(error));
    }

    if (!hardware_up) {
        (void)fprintf(stderr, "fcn_node: %s\n", error);
        posix_sim_close();
        posix_platform_shutdown();
        return EXIT_FAILURE;
    }

    LOG_INF("main", "CAN on %s at %lu bit/s, simulation control %s, sensor seed %lu",
            options.can_spec, (unsigned long)options.bitrate,
            (options.sim_port != 0U) ? "on" : "off", (unsigned long)options.sensor_seed);
    app_start(&options.app);

    const uint64_t start_ns = plat_time_ns();

    posix_platform_run();

    const uint64_t elapsed_ns = plat_time_ns() - start_ns;

    posix_can_stop();
    posix_sim_close();
    LOG_INF("main", "scheduler stopped");
    posix_platform_shutdown();

    const int transport_error = posix_can_transport_error();
    const bool stats_written =
        (options.stats_path == NULL) || write_stats(options.stats_path, &options, elapsed_ns);

    if (!stats_written) {
        (void)fprintf(stderr, "cannot write %s\n", options.stats_path);
    }

    /* The run was stopped because the bus went away (posix_can.c): that is what the exit
     * status says, whether or not the statistics could be written. */
    if (transport_error != 0) {
        (void)fprintf(stderr, "fcn_node: the CAN transport %s went away: %s\n", options.can_spec,
                      strerror(transport_error));
        return POSIX_EXIT_CAN_TRANSPORT_LOST;
    }

    return stats_written ? EXIT_SUCCESS : EXIT_FAILURE;
}
