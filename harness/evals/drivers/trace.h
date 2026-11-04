#ifndef AH_EVAL_TRACE_H
#define AH_EVAL_TRACE_H
#include "ah/ah.h"
typedef struct {
    ah_provider inner;
    const char *effort;
    bool omit_cache_key, diagnostics, trace;
    char *comparison_id;
} eval_model;
ah_provider eval_model_provider(eval_model *model);
void eval_model_close(eval_model *model);
#endif
