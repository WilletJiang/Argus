#ifndef AH_CLI_H
#define AH_CLI_H
#include "ah/ah.h"
#include "yyjson.h"
#include <pthread.h>
#include <stdatomic.h>
typedef struct {
    ah_engine *engine;
    pthread_mutex_t output;
    bool json_events;
    bool rpc;
} cli_context;
void cli_observe(void *context, const ah_event *event);
void cli_print(cli_context *cli, const char *json);
int cli_rpc(cli_context *cli);
char *cli_load_token(const char *path);
bool cli_signals_start(ah_engine *engine);
int cli_shutdown_fd(void);
void cli_signals_stop(void);
#endif
