#include "local_internal.h"
#include <ctype.h>
#include <stdio.h>

ah_local_check *ah_local_check_find(ah_local_tools *tools, const char *name) {
    for (size_t i = 0; name && i < tools->check_count; i++)
        if (!strcmp(tools->checks[i].name, name))
            return &tools->checks[i];
    return NULL;
}

ah_code ah_local_tools_add_check(ah_local_tools *tools, const ah_check_config *config,
                                 ah_error *error) {
    if (!tools || !config || !tools->enable_workflows || tools->registered ||
        tools->check_count == 16 || !config->name || !*config->name || strlen(config->name) > 64 ||
        !config->description || strlen(config->description) > 1024 || !config->argv_json ||
        !config->inputs_json || !config->timeout_ms || config->timeout_ms > 3600000 ||
        (config->cacheable && (!config->environment_revision || !*config->environment_revision)) ||
        (config->environment_revision && strlen(config->environment_revision) > 1024) ||
        ah_local_check_find(tools, config->name))
        return ah_fail(error, AH_INVALID, "Invalid, duplicate or late check preset");
    for (const unsigned char *p = (const unsigned char *)config->name; *p; p++)
        if (!isalnum(*p) && *p != '_')
            return ah_fail(error, AH_INVALID, "Check names must be alphanumeric or underscore");
    if (strlen(config->argv_json) > 131072 || strlen(config->inputs_json) > 131072)
        return ah_fail(error, AH_LIMIT, "Check definition too large");
    yyjson_doc *argv = yyjson_read(config->argv_json, strlen(config->argv_json), 0),
               *inputs = yyjson_read(config->inputs_json, strlen(config->inputs_json), 0);
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    if (argv)
        yyjson_mut_obj_add_val(doc, root, "argv",
                               yyjson_val_mut_copy(doc, yyjson_doc_get_root(argv)));
    char *arguments = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    ah_code code = argv && inputs && arguments
                       ? ah_schema_validate(ah_exec_schema, arguments, error)
                       : AH_INVALID;
    yyjson_val *list = inputs ? yyjson_doc_get_root(inputs) : NULL;
    if (!yyjson_is_arr(list) || !yyjson_arr_size(list) || yyjson_arr_size(list) > 128)
        code = AH_INVALID;
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(list, i, n, item) {
        const char *text = yyjson_get_str(item);
        size_t length = yyjson_get_len(item);
        if (!text || !length || length > 4096 || memchr(text, 0, length) || text[0] == '/')
            code = AH_INVALID;
    }
    yyjson_doc_free(argv);
    yyjson_doc_free(inputs);
    if (code != AH_OK) {
        free(arguments);
        return ah_fail(error, code, "Invalid check argv or explicit dependency paths");
    }
    ah_local_check check = {.arguments = arguments,
                            .timeout_ms = config->timeout_ms,
                            .cacheable = config->cacheable,
                            .approval = config->requires_approval};
    check.name = ah_strdup(config->name);
    check.description = ah_strdup(config->description);
    check.inputs = ah_strdup(config->inputs_json);
    check.environment = ah_strdup(config->environment_revision ? config->environment_revision : "");
    if (!check.name || !check.description || !check.inputs || !check.environment) {
        free(check.name);
        free(check.description);
        free(check.arguments);
        free(check.inputs);
        free(check.environment);
        return ah_fail(error, AH_IO, "Out of memory");
    }
    ah_hash hash;
    ah_hash_init(&hash);
    ah_hash_update(&hash, check.name, strlen(check.name) + 1);
    ah_hash_update(&hash, check.arguments, strlen(check.arguments) + 1);
    ah_hash_update(&hash, check.inputs, strlen(check.inputs) + 1);
    ah_hash_update(&hash, check.environment, strlen(check.environment) + 1);
    char policy[64];
    int policy_size =
        snprintf(policy, sizeof(policy), "%llu:%u:%u", (unsigned long long)check.timeout_ms,
                 check.cacheable, check.approval);
    ah_hash_update(&hash, policy, (size_t)policy_size);
    ah_hash_finish(&hash, check.definition);
    tools->checks[tools->check_count++] = check;
    ah_error_clear(error);
    return AH_OK;
}

ah_code ah_local_checks_register(ah_local_tools *tools, ah_engine *engine, ah_error *error) {
    if (!tools->check_count)
        return AH_OK;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *list = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, list);
    bool approval = false;
    for (size_t i = 0; i < tools->check_count; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, item, "name", tools->checks[i].name);
        yyjson_mut_obj_add_strcpy(doc, item, "description", tools->checks[i].description);
        yyjson_mut_arr_append(list, item);
        approval |= tools->checks[i].approval;
    }
    char *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    if (!json)
        return ah_fail(error, AH_IO, "Cannot describe checks");
    const char *prefix =
        "Run host-defined checks for the current file revisions. Choose preset names: ";
    char *description = malloc(strlen(prefix) + strlen(json) + 1);
    if (!description) {
        free(json);
        return ah_fail(error, AH_IO, "Out of memory");
    }
    strcpy(description, prefix);
    strcat(description, json);
    free(json);
    ah_tool tool = {
        "check", description, ah_checks_schema, AH_EFFECT_EXTERNAL, approval, ah_local_run_checks,
        tools,   false};
    ah_code code = ah_register_tool(engine, &tool, error);
    free(description);
    return code;
}

void ah_local_checks_free(ah_local_tools *tools) {
    for (size_t i = 0; i < tools->check_count; i++) {
        ah_local_check *c = &tools->checks[i];
        free(c->name);
        free(c->description);
        free(c->arguments);
        free(c->inputs);
        free(c->environment);
        free(c->cached_result);
    }
}
