/* SPDX-License-Identifier: MIT */
/*
 * Host entry point of the node: parses the command line, brings up the POSIX platform layer,
 * starts the application and, after an orderly stop, writes the run's statistics as JSON.
 *
 * Everything here runs on the main thread before the scheduler starts or after it has ended,
 * which is why stdio streams are used freely; tasks never write to one.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app.h"
#include "posix_platform.h"

#ifndef FCN_VERSION
#define FCN_VERSION "unknown"
#endif

typedef struct {
    posix_config_t platform;
    app_config_t app;
    const char *stats_path;
} node_options_t;

static void print_usage(const char *program)
{
    (void)printf("usage: %s [options]\n"
                 "  --duration-s N         stop after N seconds of tick time (default and 0: run\n"
                 "                         until SIGINT or SIGTERM)\n"
                 "  --stats-json PATH      write run statistics to PATH on an orderly stop\n"
                 "  --tick-mode MODE       locked (default) or stock\n"
                 "  --irq-mode MODE        signal (default) or poll\n"
                 "  --log-file PATH        write the log to PATH instead of stderr\n"
                 "  --log-level LEVEL      error, warn, info (default) or debug\n"
                 "  --version              print the version and exit\n"
                 "  --help                 print this text and exit\n",
                 program);
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
        {"version", no_argument, NULL, OPT_VERSION},
        {"help", no_argument, NULL, OPT_HELP},
        {NULL, 0, NULL, 0}};
    int result = 0;
    int option;

    while ((result == 0) && ((option = getopt_long(argc, argv, "", long_options, NULL)) != -1)) {
        switch (option) {
        case OPT_DURATION: {
            char *end = NULL;
            const unsigned long value = strtoul(optarg, &end, 10);

            if ((end == optarg) || (*end != '\0') || (value > 86400UL)) {
                (void)fprintf(stderr, "invalid --duration-s: %s\n", optarg);
                result = 2;
            } else {
                options->app.run_seconds = (uint32_t)value;
            }
            break;
        }
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

    (void)fprintf(file, "{\n  \"schema\": \"fcn.node_stats/1\",\n");
    (void)fprintf(file, "  \"version\": \"%s\",\n", FCN_VERSION);
    (void)fprintf(file, "  \"kernel\": \"%s\",\n", tskKERNEL_VERSION_NUMBER);
    (void)fprintf(file, "  \"elapsed_ns\": %llu,\n", (unsigned long long)elapsed_ns);
    (void)fprintf(file,
                  "  \"tick\": {\"mode\": \"%s\", \"rate_hz\": %u, \"count\": %llu, "
                  "\"thread_seen\": %s, \"signals_sent\": %llu, \"catch_ups\": %llu, "
                  "\"dropped\": %llu},\n",
                  (options->platform.tick_mode == POSIX_TICK_LOCKED) ? "locked" : "stock",
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
    (void)fprintf(file, "  \"run_time_total_us\": %llu,\n",
                  (unsigned long long)stats->total_run_time_us);
    (void)fprintf(file, "  \"tasks\": [");

    for (uint32_t i = 0U; i < stats->task_count; i++) {
        const node_task_stats_t *task = &stats->tasks[i];
        long long stack_peak = -1;

        for (size_t k = 0U; k < stack_count; k++) {
            if (strcmp(stacks[k].name, task->name) == 0) {
                stack_peak = (long long)stacks[k].peak_bytes;
            }
        }

        (void)fprintf(file,
                      "%s\n    {\"name\": \"%s\", \"priority\": %u, \"run_time_us\": %llu, "
                      "\"stack_peak_bytes\": %lld}",
                      (i == 0U) ? "" : ",", task->name, (unsigned int)task->priority,
                      (unsigned long long)task->run_time_us, stack_peak);
    }

    (void)fprintf(file, "\n  ]\n}\n");
    return (fclose(file) == 0);
}

int main(int argc, char **argv)
{
    node_options_t options;
    int status;

    (void)memset(&options, 0, sizeof(options));
    options.platform.tick_mode = POSIX_TICK_LOCKED;
    options.platform.irq_mode = POSIX_IRQ_SIGNAL;
    options.platform.log_level = LOG_LEVEL_INFO;

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

    posix_platform_init(&options.platform);
    LOG_INF("main", "fcn_node %s starting (FreeRTOS %s)", FCN_VERSION, tskKERNEL_VERSION_NUMBER);
    app_start(&options.app);

    const uint64_t start_ns = plat_time_ns();

    posix_platform_run();

    const uint64_t elapsed_ns = plat_time_ns() - start_ns;

    LOG_INF("main", "scheduler stopped");
    posix_platform_shutdown();

    if ((options.stats_path != NULL) && !write_stats(options.stats_path, &options, elapsed_ns)) {
        (void)fprintf(stderr, "cannot write %s\n", options.stats_path);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
