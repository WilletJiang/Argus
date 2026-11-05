#include "cli.h"
#include "ah/responses.h"
#include "ah/tools.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(void) {
    puts(
        "ah " AH_VERSION " — durable C agent runtime\n"
        "Usage: ah COMMAND [options]\n"
        "Commands: new run resume approve deny reply resolve inspect list events fork compact rpc\n"
        "  --db PATH            Session database (default ./harness.db)\n"
        "  --workspace PATH     Register bounded local file tools\n"
        "  --enable-exec        Explicitly allow process execution (not sandboxed)\n"
        "  --workflows          Add search and versioned apply_changes tools\n"
        "  --model NAME         Model for new session (required by new)\n"
        "  --instructions TEXT  Developer instructions for new session\n"
        "  --session ID         Target session\n"
        "  --prompt TEXT        Submit user text before run\n"
        "  --command-id ID      Stable idempotency key for prompt (required)\n"
        "  --operation ID       Approval/input/unknown-operation identity\n"
        "  --value JSON         Input or reconciliation result\n"
        "  --summary TEXT       Explicit context checkpoint summary\n"
        "  --keep-turns N       Recent complete turns retained by compact (default 4)\n"
        "  --endpoint URL       Full Responses endpoint (default OpenAI public endpoint)\n"
        "  --token-file PATH    Private token/SIWC JSON file; otherwise AH_ACCESS_TOKEN\n"
        "  --parallel-tools N   Concurrent opt-in read tools (default 4; 0/1 serial; max 16)\n"
        "  --model-tool-bytes N  Per-result model view (default 16384; 0 disables cap)\n"
        "  --events             Print structured execution events\n"
        "  --allow-http-loopback  Allow local HTTP fixture only\n"
        "RPC: bounded JSON lines; method/id/session fields. See docs/protocol.md.\n"
        "Secrets are never accepted on the command line. OAuth belongs to the host.");
}
int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        usage();
        return 0;
    }
    if (!strcmp(argv[1], "--version")) {
        puts(AH_VERSION);
        return 0;
    }
    const char *command = argv[1], *db = "harness.db", *workspace = NULL;
    const char *model = NULL, *instructions = "", *session = NULL, *prompt = NULL;
    const char *command_id = NULL, *operation = NULL, *value = NULL, *summary = NULL;
    const char *endpoint = "https://api.openai.com/v1/responses", *token_file = NULL;
    bool exec_enabled = false, events = false, allow_http = false, workflows = false;
    unsigned keep = 4, parallel = 4;
    size_t model_tool_bytes = 16384;
    for (int i = 2; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--workflows")) {
            workflows = true;
            continue;
        }
        if (!strcmp(arg, "--enable-exec")) {
            exec_enabled = true;
            continue;
        }
        if (!strcmp(arg, "--events")) {
            events = true;
            continue;
        }
        if (!strcmp(arg, "--allow-http-loopback")) {
            allow_http = true;
            continue;
        }
        if (i + 1 == argc) {
            fputs("Missing option value\n", stderr);
            return 2;
        }
        const char *v = argv[++i];
        if (!strcmp(arg, "--db"))
            db = v;
        else if (!strcmp(arg, "--workspace"))
            workspace = v;
        else if (!strcmp(arg, "--model"))
            model = v;
        else if (!strcmp(arg, "--instructions"))
            instructions = v;
        else if (!strcmp(arg, "--session"))
            session = v;
        else if (!strcmp(arg, "--prompt"))
            prompt = v;
        else if (!strcmp(arg, "--command-id"))
            command_id = v;
        else if (!strcmp(arg, "--operation"))
            operation = v;
        else if (!strcmp(arg, "--value"))
            value = v;
        else if (!strcmp(arg, "--summary"))
            summary = v;
        else if (!strcmp(arg, "--endpoint"))
            endpoint = v;
        else if (!strcmp(arg, "--token-file"))
            token_file = v;
        else if (!strcmp(arg, "--parallel-tools") || !strcmp(arg, "--model-tool-bytes")) {
            char *end = NULL;
            errno = 0;
            unsigned long number = strtoul(v, &end, 10);
            bool concurrency = !strcmp(arg, "--parallel-tools");
            if (errno || !*v || *v == '-' || *end ||
                (concurrency ? number > 16 : number != 0 && number < 1024)) {
                fprintf(stderr, "Invalid %s\n", arg);
                return 2;
            }
            if (concurrency)
                parallel = (unsigned)number;
            else
                model_tool_bytes = (size_t)number;
        } else if (!strcmp(arg, "--keep-turns")) {
            char *end = NULL;
            errno = 0;
            unsigned long number = strtoul(v, &end, 10);
            if (errno || !*v || *end || number > 10000) {
                fputs("Invalid keep-turns\n", stderr);
                return 2;
            }
            keep = (unsigned)number;
        } else {
            fputs("Unknown option\n", stderr);
            return 2;
        }
    }
    if ((exec_enabled || workflows) && !workspace) {
        fputs("--enable-exec/--workflows require --workspace\n", stderr);
        return 2;
    }
    if (prompt && !command_id) {
        fputs("--prompt requires --command-id\n", stderr);
        return 2;
    }
    const char *commands[] = {"new",     "run",  "resume", "approve", "deny",    "reply", "resolve",
                              "inspect", "list", "events", "fork",    "compact", "rpc"};
    bool known = false;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (!strcmp(command, commands[i]))
            known = true;
    if (!known) {
        fputs("Unknown command\n", stderr);
        return 2;
    }
    if (!strcmp(command, "new") && !model) {
        fputs("--model is required\n", stderr);
        return 2;
    }
    bool standalone =
        !strcmp(command, "new") || !strcmp(command, "list") || !strcmp(command, "rpc");
    if (!standalone && !session) {
        fputs("--session is required\n", stderr);
        return 2;
    }
    char *token = cli_load_token(token_file);
    if (token_file && !token) {
        fputs("Cannot read private token file (regular, owner-only permissions required)\n",
              stderr);
        return 2;
    }
    ah_error error = {0};
    ah_responses *responses = NULL;
    ah_responses_config pc;
    ah_responses_config_init(&pc);
    pc.endpoint = endpoint;
    pc.bearer_token = token;
    pc.allow_loopback_http = allow_http;
    ah_code code = ah_responses_open(&pc, &responses, &error);
    if (token) {
        size_t n = strlen(token);
        volatile char *p = token;
        for (size_t i = 0; i < n; i++)
            p[i] = 0;
        free(token);
    }
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error.message);
        return 1;
    }
    cli_context cli = {.json_events = events, .rpc = !strcmp(command, "rpc")};
    pthread_mutex_init(&cli.output, NULL);
    ah_config config;
    ah_config_init(&config);
    config.database_path = db;
    config.max_parallel_tools = parallel;
    config.max_model_tool_bytes = model_tool_bytes;
    config.provider = ah_responses_provider(responses);
    config.observer = cli_observe;
    config.observer_context = &cli;
    code = ah_open(&config, &cli.engine, &error);
    ah_local_tools *tools = NULL;
    if (code == AH_OK && workspace) {
        ah_local_tools_config tc = {.workspace = workspace,
                                    .enable_exec = exec_enabled,
                                    .approve_writes = true,
                                    .enable_workflows = workflows};
        code = ah_local_tools_open(&tc, &tools, &error);
        if (code == AH_OK)
            code = ah_local_tools_register(tools, cli.engine, &error);
    }
    bool watching = code == AH_OK && cli_signals_start(cli.engine);
    if (code == AH_OK && !watching) {
        code = AH_IO;
        snprintf(error.message, sizeof(error.message), "Cannot install cancellation handler");
    }
    char id[AH_ID_CAP] = {0};
    char *json = NULL;
    if (code == AH_OK) {
        if (cli.rpc) {
            int rc = cli_rpc(&cli);
            if (rc)
                code = AH_IO;
        } else if (!strcmp(command, "new")) {
            code = ah_session_create(cli.engine, model, instructions, id, &error);
            session = id;
        } else if (!strcmp(command, "list"))
            code = ah_sessions_json(cli.engine, &json, &error);
        else if (!strcmp(command, "inspect"))
            code = ah_session_json(cli.engine, session, &json, &error);
        else if (!strcmp(command, "events")) {
            cli.json_events = true;
            code = ah_events(cli.engine, session, 0, cli_observe, &cli, &error);
        } else if (!strcmp(command, "resume"))
            code = ah_resume(cli.engine, session, &error);
        else if (!strcmp(command, "approve") || !strcmp(command, "deny"))
            code = ah_decide(cli.engine, session, operation, !strcmp(command, "approve"), &error);
        else if (!strcmp(command, "reply"))
            code = ah_reply(cli.engine, session, operation, value, &error);
        else if (!strcmp(command, "resolve"))
            code = ah_resolve(cli.engine, session, operation, value, false, &error);
        else if (!strcmp(command, "fork")) {
            code = ah_fork(cli.engine, session, id, &error);
            session = id;
        } else if (!strcmp(command, "compact"))
            code = ah_compact(cli.engine, session, summary, keep, &error);
        if (code == AH_OK && prompt)
            code = ah_submit(cli.engine, session, command_id, prompt, &error);
        if (code == AH_OK && !strcmp(command, "run"))
            code = ah_run(cli.engine, session, &error);
        if (!cli.rpc && !json && session && code == AH_OK)
            code = ah_session_json(cli.engine, session, &json, &error);
        if (json) {
            cli_print(&cli, json);
            ah_free(json);
        }
    }
    if (watching)
        cli_signals_stop();
    ah_close(cli.engine);
    ah_local_tools_close(tools);
    ah_responses_close(responses);
    pthread_mutex_destroy(&cli.output);
    if (code != AH_OK)
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error.message);
    return code == AH_OK ? 0 : code == AH_CANCELLED ? 130 : 1;
}
