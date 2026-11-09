/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_sidecar.c - the OmniDecide v2.0.0 sidecar runtime (F6).
 *
 * The kernel + spine wrapped as a long-lived process speaking JSONL over
 * stdio, with an optional HTTP mode. Same binary, same determinism
 * guarantees: every response depends only on the request bytes and the
 * registered customization state; responses are byte-identical for
 * identical requests (the id is echoed verbatim).
 *
 *   odk_sidecar                 JSONL over stdio (one request per line)
 *   odk_sidecar --http 8130     HTTP/1.1 on 127.0.0.1:8130 (POST /rpc)
 *   odk_sidecar --http 8130 --data dir
 *                               additionally registers everything found
 *                               in dir: schemes.json, templates.json,
 *                               vocabularies.json, corpus.jsonl
 *
 * Operations (op / params):
 *   ping                     -> build info (kernel version, schema fp)
 *   register                 {kind, json, source_name?}
 *   registrations            {} -> active customization state
 *   evaluate                 {primitive, instruction, options?, context,
 *                             top_k?}
 *   classify                 {subject_ref, item_text, scope?, t}
 *   decompose                {template_id, version?, item_text, params?,
 *                             t}
 *   pin                      {request, inputs_ref?, replay_class,
 *                             timestamp}
 *   replay                   {pin_id}
 *   calibrate                {generated_from}
 *   worldstate.validate      {json}
 *   observation.ingest       {json}
 *
 * HTTP responses carry CORS headers so browser hosts can exercise the
 * same surface (no auth, no tenancy: explicitly out of scope for
 * 2.0.0).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "odk_spine.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static odk_spine *g_spine;
static odk_kernel *g_kernel;

/* ------------------------------------------------------------------ */
/* Tiny JSON request accessors (read-only, borrowed strings)           */
/* ------------------------------------------------------------------ */

static const char *req_str(const odk_json_value *params, const char *key)
{
    const odk_json_value *v;

    if (params == NULL) {
        return NULL;
    }
    v = odk_json_object_get(params, key);
    if (v == NULL || v->type != ODK_JSON_STRING) {
        return NULL;
    }
    return v->string;
}

static int req_int(const odk_json_value *params, const char *key,
                   long long *out)
{
    const odk_json_value *v;

    if (params == NULL) {
        return 0;
    }
    v = odk_json_object_get(params, key);
    if (v == NULL || v->type != ODK_JSON_NUMBER || !v->is_integer) {
        return 0;
    }
    *out = v->integer;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Response envelope                                                   */
/* ------------------------------------------------------------------ */

static void emit_response(char **out, const char *id, int ok,
                          const char *result_or_error)
{
    char *lit;
    char *payload_lit = NULL;
    const char *payload;
    size_t cap;
    char *buf;

    if (ok) {
        payload = result_or_error != NULL ? result_or_error : "null";
    } else {
        /* error payloads are raw diagnostic text: quote them exactly
         * once so the envelope stays valid JSON */
        payload_lit = odk_json_string_literal(
            result_or_error != NULL ? result_or_error : "error",
            result_or_error != NULL ? strlen(result_or_error) : 5);
        payload = payload_lit != NULL ? payload_lit : "\"error\"";
    }
    cap = strlen(payload) + 64;
    buf = malloc(cap);
    if (buf == NULL) {
        free(payload_lit);
        *out = NULL;
        return;
    }
    lit = odk_json_string_literal(id != NULL ? id : "0",
                                  id != NULL ? strlen(id) : 1);
    snprintf(buf, cap,
             ok ? "{\"id\":%s,\"ok\":true,\"result\":%s}"
                : "{\"id\":%s,\"ok\":false,\"error\":%s}",
             lit != NULL ? lit : "\"0\"", payload);
    free(lit);
    free(payload_lit);
    *out = buf;
}

/* ------------------------------------------------------------------ */
/* Operation dispatch                                                  */
/* ------------------------------------------------------------------ */

static char *op_register(const odk_json_value *params, int *ok)
{
    const char *kind = req_str(params, "kind");
    const char *json = req_str(params, "json");
    const char *source = req_str(params, "source_name");
    odk_spine_error err;

    if (kind == NULL || json == NULL) {
        *ok = 0;
        return strdup("\"register: kind and json are required\"");
    }
    if (odk_spine_register_json(g_spine, kind, json, source, &err) ==
        ODK_SPINE_OK) {
        *ok = 1;
        return strdup("{\"registered\":true}");
    }
    *ok = 0;
    return strdup(err.text);
}

static char *op_registrations(const odk_json_value *params, int *ok)
{
    odk_spine_error err;
    odk_spine_registration_ref_list regs;

    (void)params;
    if (odk_spine_registrations(g_spine, &regs, &err) != ODK_SPINE_OK) {
        *ok = 0;
        return strdup("\"out of memory\"");
    }
    {
        size_t i;
        char *out = NULL;
        size_t len = 0, cap = 0;

#define APPEND(strlit) \
    do { \
        size_t n_ = strlen(strlit); \
        if (len + n_ + 1 > cap) { \
            size_t ncap_ = cap == 0 ? 256 : cap * 2; \
            char *nd_; \
            while (ncap_ < len + n_ + 1) { \
                ncap_ *= 2; \
            } \
            nd_ = realloc(out, ncap_); \
            if (nd_ == NULL) { \
                free(out); \
                odk_spine_registration_ref_list_release(&regs); \
                *ok = 0; \
                return NULL; \
            } \
            out = nd_; \
            cap = ncap_; \
        } \
        memcpy(out + len, strlit, n_); \
        len += n_; \
        out[len] = '\0'; \
    } while (0)

        APPEND("{\"registrations\":[");
        for (i = 0; i < regs.count; i++) {
            char *kind_lit =
                odk_json_string_literal(regs.items[i].kind,
                                        strlen(regs.items[i].kind));
            char *id_lit = odk_json_string_literal(regs.items[i].id,
                                                   strlen(regs.items[i].id));
            char *hash_lit = odk_json_string_literal(
                regs.items[i].content_hash,
                strlen(regs.items[i].content_hash));
            char row[640];

            snprintf(row, sizeof(row),
                     "%s{\"kind\":%s,\"id\":%s,\"version\":%lld,"
                     "\"content_hash\":%s}",
                     i == 0 ? "" : ",", kind_lit != NULL ? kind_lit : "\"\"",
                     id_lit != NULL ? id_lit : "\"\"",
                     (long long)regs.items[i].version,
                     hash_lit != NULL ? hash_lit : "\"\"");
            APPEND(row);
            free(kind_lit);
            free(id_lit);
            free(hash_lit);
        }
        APPEND("]}");
#undef APPEND
        odk_spine_registration_ref_list_release(&regs);
        *ok = 1;
        return out;
    }
}

static char *op_evaluate(const odk_json_value *params, int *ok)
{
    char *request_json;
    char *decision = NULL;
    double confidence = 0.0;
    char *result_json = NULL;
    odk_spine_error err;
    char *out;
    size_t cap;
    char *id_lit;
    double d;
    char conf_lit[40];

    /* params ARE the request document; re-serialize with the
     * deterministic writer (key order is insertion order) */
    request_json = odk_json_dump(params);
    if (odk_spine_evaluate_request(g_kernel, request_json, &decision,
                                   &confidence, &result_json,
                                   &err) != ODK_SPINE_OK) {
        free(request_json);
        *ok = 0;
        return strdup(err.text);
    }
    id_lit = odk_json_string_literal(decision, strlen(decision));
    d = confidence;
    {
        char *cl = odk_json_number_literal(d);

        snprintf(conf_lit, sizeof(conf_lit), "%s", cl != NULL ? cl : "0.0");
        free(cl);
    }
    cap = strlen(result_json) + strlen(id_lit != NULL ? id_lit : "") + 64;
    out = malloc(cap);
    if (out == NULL) {
        free(request_json);
        free(decision);
        free(result_json);
        free(id_lit);
        *ok = 0;
        return NULL;
    }
    snprintf(out, cap, "{\"decision\":%s,\"confidence\":%s,\"result\":%s}",
             id_lit != NULL ? id_lit : "\"\"", conf_lit, result_json);
    free(request_json);
    free(decision);
    free(result_json);
    free(id_lit);
    *ok = 1;
    return out;
}

static char *op_classify(const odk_json_value *params, int *ok)
{
    const char *subject = req_str(params, "subject_ref");
    const char *item = req_str(params, "item_text");
    const char *scope = req_str(params, "scope");
    long long t = 0;
    odk_spine_classification_result result;
    odk_spine_error err;
    char *json;

    if (subject == NULL || item == NULL) {
        *ok = 0;
        return strdup("\"classify: subject_ref and item_text are required\"");
    }
    req_int(params, "t", &t);
    if (odk_spine_classify(g_spine, subject, item, scope, (int64_t)t,
                           &result, &err) != ODK_SPINE_OK) {
        *ok = 0;
        return strdup(err.text);
    }
    json = odk_spine_classification_result_to_json(&result);
    odk_spine_classification_result_release(&result);
    if (json == NULL) {
        *ok = 0;
        return NULL;
    }
    *ok = 1;
    return json;
}

static char *op_decompose(const odk_json_value *params, int *ok)
{
    const char *template_id = req_str(params, "template_id");
    const char *item = req_str(params, "item_text");
    const char *params_json = req_str(params, "params");
    long long version = -1;
    long long t = 0;
    odk_spine_components comps;
    odk_spine_error err;
    char *json;
    char *params_owned = NULL;

    if (template_id == NULL || item == NULL) {
        *ok = 0;
        return strdup("\"decompose: template_id and item_text are required\"");
    }
    /* params may arrive as an object; serialize with the deterministic
     * writer so the decomposition bytes stay stable */
    if (params_json == NULL) {
        const odk_json_value *pj = odk_json_object_get(params, "params");

        if (pj != NULL && pj->type == ODK_JSON_OBJECT) {
            params_owned = odk_json_dump(pj);
            params_json = params_owned;
        }
    }
    req_int(params, "version", &version);
    req_int(params, "t", &t);
    if (odk_spine_decompose(g_spine, template_id, (int64_t)version, item,
                            params_json, (int64_t)t, &comps, &err) !=
        ODK_SPINE_OK) {
        free(params_owned);
        *ok = 0;
        return strdup(err.text);
    }
    free(params_owned);
    json = odk_spine_components_to_json(&comps);
    odk_spine_components_release(&comps);
    if (json == NULL) {
        *ok = 0;
        return NULL;
    }
    *ok = 1;
    return json;
}

static char *op_pin(const odk_json_value *params, int *ok)
{
    const char *request = req_str(params, "request");
    const char *inputs_ref = req_str(params, "inputs_ref");
    const char *replay_class = req_str(params, "replay_class");
    long long timestamp = 0;
    odk_spine_decision_pin pin;
    odk_spine_error err;
    char *json;
    char *request_owned = NULL;
    char *class_owned = NULL;

    /* request may be a JSON object (serialized with the deterministic
     * writer) or a pre-serialized string */
    if (request == NULL) {
        const odk_json_value *req =
            odk_json_object_get(params, "request");

        if (req != NULL && req->type == ODK_JSON_OBJECT) {
            request_owned = odk_json_dump(req);
            request = request_owned;
        }
    }
    if (replay_class == NULL) {
        const odk_json_value *rc = odk_json_object_get(params, "replay_class");

        if (rc != NULL && rc->type == ODK_JSON_STRING) {
            replay_class = rc->string;
        }
    }
    if (request == NULL || replay_class == NULL) {
        free(request_owned);
        *ok = 0;
        return strdup("\"pin: request and replay_class are required\"");
    }
    req_int(params, "timestamp", &timestamp);
    if (odk_spine_pin(g_spine, g_kernel, request, inputs_ref, replay_class,
                      (int64_t)timestamp, &pin, &err) != ODK_SPINE_OK) {
        free(request_owned);
        *ok = 0;
        return strdup(err.text);
    }
    free(request_owned);
    (void)class_owned;
    json = odk_spine_decision_pin_to_json(&pin);
    odk_spine_decision_pin_release(&pin);
    if (json == NULL) {
        *ok = 0;
        return NULL;
    }
    *ok = 1;
    return json;
}

static char *op_replay(const odk_json_value *params, int *ok)
{
    const char *pin_id = req_str(params, "pin_id");
    odk_spine_error err;
    char *report = NULL;

    if (pin_id == NULL) {
        *ok = 0;
        return strdup("\"replay: pin_id is required\"");
    }
    if (odk_spine_replay(g_spine, g_kernel, pin_id, &report, &err) !=
        ODK_SPINE_OK) {
        *ok = 0;
        return strdup(err.text);
    }
    *ok = 1;
    return report;
}

static char *op_calibrate(const odk_json_value *params, int *ok)
{
    const char *from = req_str(params, "generated_from");
    odk_spine_calibration_report report;
    odk_spine_error err;
    char *json;

    if (odk_spine_calibrate(g_spine, from, &report, &err) != ODK_SPINE_OK) {
        *ok = 0;
        return strdup(err.text);
    }
    json = odk_spine_calibration_report_to_json(&report);
    odk_spine_calibration_report_release(&report);
    if (json == NULL) {
        *ok = 0;
        return NULL;
    }
    *ok = 1;
    return json;
}

static char *op_worldstate(const odk_json_value *params, int *ok)
{
    const char *json = req_str(params, "json");
    odk_spine_error err;

    if (json == NULL) {
        *ok = 0;
        return strdup("\"worldstate.validate: json is required\"");
    }
    if (odk_spine_worldstate_validate(json, &err) == ODK_SPINE_OK) {
        *ok = 1;
        return strdup("{\"valid\":true}");
    }
    *ok = 0;
    return strdup(err.text);
}

static char *op_observation_ingest(const odk_json_value *params, int *ok)
{
    const char *json = req_str(params, "json");
    odk_spine_error err;

    if (json == NULL) {
        *ok = 0;
        return strdup("\"observation.ingest: json is required\"");
    }
    if (odk_spine_observation_ingest(g_spine, json, &err) == ODK_SPINE_OK) {
        *ok = 1;
        return strdup("{\"ingested\":true}");
    }
    *ok = 0;
    return strdup(err.text);
}

static char *op_ping(const odk_json_value *params, int *ok)
{
    char out[256];

    (void)params;
    snprintf(out, sizeof(out),
             "{\"pong\":true,\"kernel_version\":\"%s\","
             "\"schema_fingerprint\":\"%s\"}",
             ODK_SPINE_KERNEL_VERSION, ODK_SPINE_SCHEMA_FINGERPRINT);
    *ok = 1;
    return strdup(out);
}

static char *dispatch(const char *op, const odk_json_value *params, int *ok)
{
    if (op == NULL) {
        *ok = 0;
        return strdup("\"op is required\"");
    }
    if (strcmp(op, "ping") == 0) {
        return op_ping(params, ok);
    }
    if (strcmp(op, "register") == 0) {
        return op_register(params, ok);
    }
    if (strcmp(op, "registrations") == 0) {
        return op_registrations(params, ok);
    }
    if (strcmp(op, "evaluate") == 0) {
        return op_evaluate(params, ok);
    }
    if (strcmp(op, "classify") == 0) {
        return op_classify(params, ok);
    }
    if (strcmp(op, "decompose") == 0) {
        return op_decompose(params, ok);
    }
    if (strcmp(op, "pin") == 0) {
        return op_pin(params, ok);
    }
    if (strcmp(op, "replay") == 0) {
        return op_replay(params, ok);
    }
    if (strcmp(op, "calibrate") == 0) {
        return op_calibrate(params, ok);
    }
    if (strcmp(op, "worldstate.validate") == 0) {
        return op_worldstate(params, ok);
    }
    if (strcmp(op, "observation.ingest") == 0) {
        return op_observation_ingest(params, ok);
    }
    *ok = 0;
    {
        char *out = malloc(strlen(op) + 32);

        if (out != NULL) {
            snprintf(out, strlen(op) + 32, "\"unknown op '%s'\"", op);
        }
        return out;
    }
}

/* Handle one request line (stdio) or body (HTTP); returns malloc'd
 * response document. */
static char *handle_request(const char *line)
{
    odk_json_value *root;
    odk_error jerr;
    const odk_json_value *params;
    const char *id = "0";
    const char *op;
    int ok = 0;
    char *result = NULL;
    char *response = NULL;

    root = odk_json_parse(line, strlen(line), &jerr);
    if (root == NULL) {
        emit_response(&response, "0", 0, "\"invalid request JSON\"");
        return response;
    }
    if (root->type != ODK_JSON_OBJECT) {
        odk_json_free(root);
        emit_response(&response, "0", 0, "\"request must be an object\"");
        return response;
    }
    {
        const odk_json_value *v = odk_json_object_get(root, "id");

        if (v != NULL && v->type == ODK_JSON_STRING) {
            id = v->string;
        }
    }
    op = req_str(root, "op");
    params = odk_json_object_get(root, "params");
    if (params == NULL || params->type != ODK_JSON_OBJECT) {
        params = NULL;
    }
    result = dispatch(op, params, &ok);
    emit_response(&response, id, ok, result != NULL ? result : "null");
    free(result);
    odk_json_free(root);
    return response;
}

/* ------------------------------------------------------------------ */
/* stdio mode                                                          */
/* ------------------------------------------------------------------ */

static void run_stdio(void)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    for (;;) {
        n = getline(&line, &cap, stdin);
        if (n < 0) {
            break;
        }
        if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        {
            char *response = handle_request(line);

            if (response != NULL) {
                fputs(response, stdout);
                fputc('\n', stdout);
                fflush(stdout);
                free(response);
            }
        }
    }
    free(line);
}

/* ------------------------------------------------------------------ */
/* HTTP mode                                                           */
/* ------------------------------------------------------------------ */

static void http_serve(int port)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    int one = 1;
    char *body = NULL;
    size_t body_cap = 0;

    if (server < 0) {
        perror("socket");
        exit(2);
    }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(2);
    }
    if (listen(server, 4) < 0) {
        perror("listen");
        exit(2);
    }
    printf("listening on 127.0.0.1:%d\n", port);
    fflush(stdout);

    for (;;) {
        int client = accept(server, NULL, NULL);
        size_t content_length = 0;
        char method[16] = {0};
        char path[256] = {0};
        size_t got = 0;
        char header[8192];

        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        /* read request head */
        {
            size_t head = 0;

            while (head < sizeof(header) - 1) {
                ssize_t r = recv(client, header + head, 1, 0);

                if (r <= 0) {
                    break;
                }
                head++;
                if (head >= 4 &&
                    strncmp(header + head - 4, "\r\n\r\n", 4) == 0) {
                    break;
                }
            }
            header[head] = '\0';
        }
        sscanf(header, "%15s %255s", method, path);
        {
            const char *cl = strstr(header, "Content-Length:");

            if (cl == NULL) {
                cl = strstr(header, "content-length:");
            }
            if (cl != NULL) {
                content_length = (size_t)strtoul(cl + 15, NULL, 10);
            }
        }
        if (content_length + 1 > body_cap) {
            body = realloc(body, content_length + 1);
            body_cap = content_length + 1;
        }
        while (got < content_length) {
            ssize_t r = recv(client, body + got, content_length - got, 0);

            if (r <= 0) {
                break;
            }
            got += (size_t)r;
        }
        body[got] = '\0';

        /* liveness probe (Content-Length computed: the body is 20 bytes) */
        if (strcmp(method, "GET") == 0 && strncmp(path, "/health", 7) == 0) {
            char resp[256];
            const char *body = "{\"status\":\"running\"}";

            snprintf(resp, sizeof(resp),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     strlen(body), body);
            send(client, resp, strlen(resp), 0);
            close(client);
            continue;
        }

        if (strcmp(method, "POST") == 0 &&
            (strncmp(path, "/rpc", 4) == 0 || strncmp(path, "/", 1) == 0)) {
            char *response = handle_request(body);
            char head[256];

            if (response == NULL) {
                response = strdup("{\"id\":\"0\",\"ok\":false,"
                                  "\"error\":\"out of memory\"}");
            }
            snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/json\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     strlen(response));
            send(client, head, strlen(head), 0);
            send(client, response, strlen(response), 0);
            free(response);
        } else if (strcmp(method, "OPTIONS") == 0) {
            const char *resp =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Connection: close\r\n\r\n";
            send(client, resp, strlen(resp), 0);
        } else {
            char resp[256];
            const char *body = "{\"error\":\"not found\"}";

            snprintf(resp, sizeof(resp),
                     "HTTP/1.1 404 Not Found\r\n"
                     "Content-Type: application/json\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     strlen(body), body);
            send(client, resp, strlen(resp), 0);
        }
        close(client);
    }
    free(body);
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int http_port = -1;
    const char *data_dir = NULL;
    int i;

    signal(SIGPIPE, SIG_IGN);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--http") == 0 && i + 1 < argc) {
            http_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        }
    }

    g_kernel = odk_kernel_create();
    g_spine = odk_spine_create();
    if (g_kernel == NULL || g_spine == NULL) {
        fprintf(stderr, "odk_sidecar: init failed\n");
        return 2;
    }

    if (data_dir != NULL) {
        static const struct {
            const char *file;
            const char *kind;
        } files[] = {
            {"schemes.json", ODK_SPINE_KIND_SCHEME_SET},
            {"templates.json", ODK_SPINE_KIND_TEMPLATE},
            {"vocabularies.json", ODK_SPINE_KIND_VOCABULARY},
            {"corpus.jsonl", ODK_SPINE_KIND_CORPUS},
            {NULL, NULL}
        };
        char path[1024];

        for (i = 0; files[i].file != NULL; i++) {
            FILE *probe;
            odk_spine_error err;

            snprintf(path, sizeof(path), "%s/%s", data_dir, files[i].file);
            probe = fopen(path, "rb");
            if (probe == NULL) {
                continue;
            }
            fclose(probe);
            if (odk_spine_register_file(g_spine, files[i].kind, path,
                                        &err) != ODK_SPINE_OK) {
                fprintf(stderr, "odk_sidecar: %s: %s\n", path, err.text);
                return 2;
            }
        }
    }

    if (http_port > 0) {
        http_serve(http_port);
    } else {
        run_stdio();
    }

    odk_kernel_free(g_kernel);
    odk_spine_free(g_spine);
    return 0;
}
