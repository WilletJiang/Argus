#include "core.h"

static void free_message(core_steering *message) {
    free(message->session);
    free(message->command);
    free(message->text);
    free(message);
}
void core_mailbox_active(ah_engine *e, const char *sid) {
    pthread_mutex_lock(&e->mailbox_lock);
    if (sid)
        memcpy(e->active_session, sid, AH_ID_CAP);
    else
        e->active_session[0] = '\0';
    pthread_mutex_unlock(&e->mailbox_lock);
}
void core_mailbox_free(ah_engine *e) {
    if (!e->mailbox_initialized)
        return;
    core_steering *message = e->mailbox_head;
    while (message) {
        core_steering *next = message->next;
        free_message(message);
        message = next;
    }
    pthread_mutex_destroy(&e->mailbox_lock);
}
ah_code ah_steer(ah_engine *e, const char *sid, const char *command, const char *text,
                 ah_error *err) {
    if (!e || !sid || !command || !*command || strlen(command) > 256 || !text || !*text ||
        strlen(text) > e->config.max_context_bytes)
        return ah_fail(err, AH_INVALID, "bounded session, command id and steering text required");
    if (strlen(sid) != AH_ID_CAP - 1)
        return ah_fail(err, AH_INVALID, "invalid session id");
    pthread_mutex_lock(&e->mailbox_lock);
    if (strcmp(e->active_session, sid)) {
        pthread_mutex_unlock(&e->mailbox_lock);
        return ah_fail(err, AH_BUSY, "steering requires the active running session");
    }
    for (core_steering *m = e->mailbox_head; m; m = m->next) {
        if (!strcmp(m->session, sid) && !strcmp(m->command, command)) {
            bool same = !strcmp(m->text, text);
            pthread_mutex_unlock(&e->mailbox_lock);
            return same ? AH_OK : ah_fail(err, AH_INVALID, "queued command id has different text");
        }
    }
    size_t bytes = strlen(sid) + strlen(command) + strlen(text) + 3;
    if (bytes > e->config.max_context_bytes ||
        e->mailbox_bytes > e->config.max_context_bytes - bytes) {
        pthread_mutex_unlock(&e->mailbox_lock);
        return ah_fail(err, AH_LIMIT, "steering mailbox byte budget exhausted");
    }
    core_steering *message = calloc(1, sizeof(*message));
    if (message) {
        message->session = ah_strdup(sid);
        message->command = ah_strdup(command);
        message->text = ah_strdup(text);
    }
    if (!message || !message->session || !message->command || !message->text) {
        if (message)
            free_message(message);
        pthread_mutex_unlock(&e->mailbox_lock);
        return ah_fail(err, AH_IO, "out of memory");
    }
    if (e->mailbox_tail)
        e->mailbox_tail->next = message;
    else
        e->mailbox_head = message;
    e->mailbox_tail = message;
    e->mailbox_bytes += bytes;
    pthread_mutex_unlock(&e->mailbox_lock);
    ah_error_clear(err);
    return AH_OK;
}
static char *ack_json(const core_steering *message, bool duplicate) {
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return NULL;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_strcpy(d, o, "command_id", message->command);
    yyjson_mut_obj_add_strcpy(d, o, "text", message->text);
    yyjson_mut_obj_add_bool(d, o, "duplicate", duplicate);
    char *json = ah_mut_write(d);
    ah_mut_doc_free(d);
    return json;
}
static ah_code persist_message(ah_engine *e, core_session *session, core_steering *message,
                               bool *added, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e, "SELECT text FROM commands WHERE session_id=? AND id=?", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, message->session, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, message->command, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    bool exists = step == SQLITE_ROW,
         same = exists && !strcmp((const char *)sqlite3_column_text(s, 0), message->text);
    sqlite3_finalize(s);
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        return ah_fail(err, AH_IO, "steering command lookup failed");
    char *ack = ack_json(message, exists);
    if (!ack)
        return ah_fail(err, AH_IO, "out of memory");
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK && !exists) {
        rc = core_prepare(e, "INSERT INTO commands(session_id,id,text) VALUES(?,?,?)", &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, message->session, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, message->command, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, message->text, -1, SQLITE_TRANSIENT);
            rc = core_done(e, s, err);
        }
        yyjson_mut_doc *d = ah_mut_doc_new();
        if (!d)
            rc = ah_fail(err, AH_IO, "out of memory");
        char *json = NULL;
        if (d) {
            yyjson_mut_val *o = yyjson_mut_obj(d);
            yyjson_mut_doc_set_root(d, o);
            yyjson_mut_obj_add_str(d, o, "role", "user");
            yyjson_mut_obj_add_strcpy(d, o, "content", message->text);
            json = ah_mut_write(d);
            ah_mut_doc_free(d);
            if (!json)
                rc = ah_fail(err, AH_IO, "out of memory");
        }
        if (rc == AH_OK)
            rc = core_add_item(e, session->id, session->turn, json, err);
        free(json);
        if (rc == AH_OK)
            rc = core_state(e, session->id, AH_RUNNING, err);
    }
    if (rc == AH_OK)
        rc = core_event(e, session->id, NULL, exists && !same ? "input.rejected" : "input.steered",
                        ack, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    free(ack);
    if (rc == AH_OK && !exists) {
        *added = true;
        session->state = AH_RUNNING;
        session->completion_committed = false;
    }
    return rc;
}
ah_code core_drain_steering(ah_engine *e, core_session *session, bool terminal, ah_error *err) {
    pthread_mutex_lock(&e->mailbox_lock);
    if (terminal)
        e->active_session[0] = '\0';
    core_steering *messages = NULL, *tail = NULL, **link = &e->mailbox_head;
    while (*link) {
        core_steering *message = *link;
        if (!strcmp(message->session, session->id)) {
            *link = message->next;
            message->next = NULL;
            if (tail)
                tail->next = message;
            else
                messages = message;
            tail = message;
        } else
            link = &message->next;
    }
    e->mailbox_tail = e->mailbox_head;
    while (e->mailbox_tail && e->mailbox_tail->next)
        e->mailbox_tail = e->mailbox_tail->next;
    pthread_mutex_unlock(&e->mailbox_lock);
    ah_code rc = AH_OK;
    bool added = false;
    while (messages) {
        rc = persist_message(e, session, messages, &added, err);
        if (rc != AH_OK) {
            pthread_mutex_lock(&e->mailbox_lock);
            tail->next = e->mailbox_head;
            e->mailbox_head = messages;
            if (!e->mailbox_tail)
                e->mailbox_tail = tail;
            pthread_mutex_unlock(&e->mailbox_lock);
            break;
        }
        core_steering *next = messages->next;
        size_t bytes =
            strlen(messages->session) + strlen(messages->command) + strlen(messages->text) + 3;
        pthread_mutex_lock(&e->mailbox_lock);
        e->mailbox_bytes -= bytes;
        pthread_mutex_unlock(&e->mailbox_lock);
        free_message(messages);
        messages = next;
    }
    if (terminal && added && rc == AH_OK)
        core_mailbox_active(e, session->id);
    return rc;
}
