/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_spine_registry.c - the F7 application extension API: registration
 * of scheme sets, decomposition templates, vocabularies, golden corpora
 * and brain snapshots.
 *
 * Lifecycle: register -> validate (schema + referential integrity) ->
 * activate atomically. Re-registration is a versioned append, never an
 * in-place mutation. No domain content is compiled in; the shipped build
 * contains zero registered customizations.
 *
 * Determinism: entries are kept in registration order; lookups iterate
 * arrays (never hash order); identical registered data yields identical
 * results.
 */

#include "odk_spine_private.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

char *odk_spine_read_file(const char *path, size_t *out_len)
{
    FILE *f;
    long sz;
    char *buf;

    if (path == NULL) {
        return NULL;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    if (out_len != NULL) {
        *out_len = (size_t)sz;
    }
    return buf;
}

/* Is this string a safe id: non-empty printable ASCII, no whitespace? */
static int id_ok(const char *s)
{
    size_t i, n;

    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    n = strlen(s);
    if (n > 128) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c >= 0x7F) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Referential-integrity validation                                    */
/* ------------------------------------------------------------------ */

odk_spine_status odk_spine_validate_scheme(const odk_spine_scheme *s,
                                           odk_spine_error *err)
{
    size_t i, k;

    if (!id_ok(s->id)) {
        set_err(err, "scheme: invalid id");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (s->version < 1) {
        set_errf(err, "scheme %s: version must be >= 1", s->id);
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (s->valid_to != -1 && s->valid_from > s->valid_to) {
        set_errf(err, "scheme %s v%lld: empty validity interval", s->id,
                 (long long)s->version);
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (s->default_label[0] == '\0') {
        set_errf(err, "scheme %s v%lld: default_label is required", s->id,
                 (long long)s->version);
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (i = 0; i < s->facet_names.count; i++) {
        for (k = 0; k < i; k++) {
            if (strcmp(s->facet_names.items[i], s->facet_names.items[k]) ==
                0) {
                set_errf(err, "scheme %s v%lld: duplicate facet '%s'", s->id,
                         (long long)s->version, s->facet_names.items[i]);
                return ODK_SPINE_ERR_REFERENTIAL;
            }
        }
    }
    for (i = 0; i < s->rules.count; i++) {
        const odk_spine_scheme_rule *rule = &s->rules.items[i];
        size_t f;

        if (rule->label[0] == '\0') {
            set_errf(err, "scheme %s v%lld: rule %zu: empty label", s->id,
                     (long long)s->version, i);
            return ODK_SPINE_ERR_SCHEMA;
        }
        if (rule->match_terms.count == 0) {
            set_errf(err, "scheme %s v%lld: rule %zu: no match terms", s->id,
                     (long long)s->version, i);
            return ODK_SPINE_ERR_REFERENTIAL;
        }
        for (f = 0; f < rule->facets.count; f++) {
            size_t n;
            int known = 0;

            for (n = 0; n < s->facet_names.count; n++) {
                if (strcmp(rule->facets.items[f].key,
                           s->facet_names.items[n]) == 0) {
                    known = 1;
                    break;
                }
            }
            if (!known) {
                set_errf(err,
                         "scheme %s v%lld: rule %zu: facet '%s' is not in "
                         "facet_names",
                         s->id, (long long)s->version, i,
                         rule->facets.items[f].key);
                return ODK_SPINE_ERR_REFERENTIAL;
            }
        }
    }
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_validate_template(
    const odk_spine_decomposition_template *t, odk_spine_error *err)
{
    size_t i, k;

    if (!id_ok(t->id)) {
        set_err(err, "template: invalid id");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (t->version < 1) {
        set_errf(err, "template %s: version must be >= 1", t->id);
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (t->valid_to != -1 && t->valid_from > t->valid_to) {
        set_errf(err, "template %s v%lld: empty validity interval", t->id,
                 (long long)t->version);
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (i = 0; i < t->parameters.count; i++) {
        const odk_spine_template_parameter *p = &t->parameters.items[i];
        if (p->name[0] == '\0') {
            set_errf(err, "template %s v%lld: parameter %zu: empty name",
                     t->id, (long long)t->version, i);
            return ODK_SPINE_ERR_SCHEMA;
        }
        for (k = 0; k < i; k++) {
            if (strcmp(p->name, t->parameters.items[k].name) == 0) {
                set_errf(err, "template %s v%lld: duplicate parameter '%s'",
                         t->id, (long long)t->version, p->name);
                return ODK_SPINE_ERR_REFERENTIAL;
            }
        }
    }
    for (i = 0; i < t->component_specs.count; i++) {
        const odk_spine_component_spec *cs = &t->component_specs.items[i];
        if (cs->name[0] == '\0') {
            set_errf(err, "template %s v%lld: component %zu: empty name",
                     t->id, (long long)t->version, i);
            return ODK_SPINE_ERR_SCHEMA;
        }
        if (cs->max_items < 0) {
            set_errf(err, "template %s v%lld: component %s: max_items < 0",
                     t->id, (long long)t->version, cs->name);
            return ODK_SPINE_ERR_SCHEMA;
        }
        for (k = 0; k < i; k++) {
            if (strcmp(cs->name, t->component_specs.items[k].name) == 0) {
                set_errf(err, "template %s v%lld: duplicate component '%s'",
                         t->id, (long long)t->version, cs->name);
                return ODK_SPINE_ERR_REFERENTIAL;
            }
        }
    }
    for (i = 0; i < t->extraction_rules.count; i++) {
        const odk_spine_extraction_rule *r = &t->extraction_rules.items[i];
        int known = 0;

        for (k = 0; k < t->component_specs.count; k++) {
            if (strcmp(r->component, t->component_specs.items[k].name) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            set_errf(err,
                     "template %s v%lld: extraction rule %zu references "
                     "unknown component '%s'",
                     t->id, (long long)t->version, i, r->component);
            return ODK_SPINE_ERR_REFERENTIAL;
        } else {
            /* kind/mode consistency of the perception contract */
            const char *kind =
                t->component_specs.items[k].kind;
            const char *want =
                strcmp(r->mode, "sentence") == 0 ? "sentences"
                : (strcmp(r->mode, "keyvalue") == 0 ? "keyvalue"
                                                    : "keywords");
            if (strcmp(kind, want) != 0) {
                set_errf(err,
                         "template %s v%lld: component '%s' has kind '%s' "
                         "but its rule mode is '%s' (expected '%s')",
                         t->id, (long long)t->version, r->component, kind,
                         r->mode, want);
                return ODK_SPINE_ERR_REFERENTIAL;
            }
        }
        if (strcmp(r->mode, "sentence") == 0 && r->window != 0) {
            set_errf(err, "template %s v%lld: sentence rule %zu: window "
                          "must be 0",
                     t->id, (long long)t->version, i);
            return ODK_SPINE_ERR_SCHEMA;
        }
        if ((strcmp(r->mode, "before") == 0 || strcmp(r->mode, "after") == 0 ||
             strcmp(r->mode, "around") == 0) && r->window < 0) {
            set_errf(err, "template %s v%lld: rule %zu: negative window",
                     t->id, (long long)t->version, i);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_validate_vocabulary(const odk_spine_vocabulary *v,
                                               odk_spine_error *err)
{
    size_t i, k, n;

    if (!id_ok(v->id)) {
        set_err(err, "vocabulary: invalid id");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (v->version < 1) {
        set_errf(err, "vocabulary %s: version must be >= 1", v->id);
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (v->valid_to != -1 && v->valid_from > v->valid_to) {
        set_errf(err, "vocabulary %s v%lld: empty validity interval", v->id,
                 (long long)v->version);
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (i = 0; i < v->groups.count; i++) {
        const odk_spine_synonym_group *g = &v->groups.items[i];
        if (g->canonical[0] == '\0') {
            set_errf(err, "vocabulary %s v%lld: group %zu: empty canonical",
                     v->id, (long long)v->version, i);
            return ODK_SPINE_ERR_SCHEMA;
        }
        if (g->terms.count == 0) {
            set_errf(err, "vocabulary %s v%lld: group %zu: no terms", v->id,
                     (long long)v->version, i);
            return ODK_SPINE_ERR_REFERENTIAL;
        }
        for (k = 0; k < g->terms.count; k++) {
            for (n = 0; n < k; n++) {
                if (strcmp(g->terms.items[k], g->terms.items[n]) == 0) {
                    set_errf(err,
                             "vocabulary %s v%lld: group %zu: duplicate "
                             "term '%s'",
                             v->id, (long long)v->version, i, g->terms.items[k]);
                    return ODK_SPINE_ERR_REFERENTIAL;
                }
            }
        }
    }
    return ODK_SPINE_OK;
}

/* ------------------------------------------------------------------ */
/* Registry plumbing                                                   */
/* ------------------------------------------------------------------ */

static char *g_kind_names[] = {
    "scheme_set",
    "decomposition_template",
    "vocabulary",
    "corpus",
    "brain_snapshot",
    NULL
};

static int kind_known(const char *kind)
{
    int i;

    for (i = 0; g_kind_names[i] != NULL; i++) {
        if (strcmp(kind, g_kind_names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Accepts one record object or an array of record objects; returns the
 * parsed DOM (caller frees) and sets *is_array. */
static odk_json_value *parse_records(const char *json, int *is_array,
                                     odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;

    *is_array = 0;
    root = odk_json_parse(json, strlen(json), &jerr);
    if (root == NULL) {
        set_errf(err, "invalid JSON: %s", jerr.text);
        return NULL;
    }
    if (root->type == ODK_JSON_ARRAY) {
        *is_array = 1;
    } else if (root->type != ODK_JSON_OBJECT) {
        set_err(err, "expected an object or an array of objects");
        odk_json_free(root);
        return NULL;
    }
    return root;
}

/* ------------------------------------------------------------------ */
/* Registration: scheme sets                                           */
/* ------------------------------------------------------------------ */

static odk_spine_status register_scheme_set(odk_spine *spine,
                                            const odk_json_value *root,
                                            const char *content_hash,
                                            const char *source_name,
                                            odk_spine_error *err)
{
    size_t n, i;

    if (root->type == ODK_JSON_OBJECT) {
        n = 1;
    } else {
        n = root->item_count;
    }

    /* validate everything first: activation is atomic */
    for (i = 0; i < n; i++) {
        odk_spine_scheme s;
        odk_spine_status st;
        const odk_json_value *rec = (root->type == ODK_JSON_OBJECT)
                                        ? root
                                        : root->items[i];

        odk_spine_scheme_init(&s);
        st = odk_spine_scheme_from_dom(rec, &s, err);
        if (st != ODK_SPINE_OK) {
            odk_spine_scheme_release(&s);
            return st;
        }
        st = odk_spine_validate_scheme(&s, err);
        if (st != ODK_SPINE_OK) {
            odk_spine_scheme_release(&s);
            return st;
        }
        /* duplicate id within the same registration batch */
        {
            size_t j;
            for (j = 0; j < spine->scheme_count; j++) {
                if (strcmp(spine->schemes[j].scheme.id, s.id) == 0 &&
                    spine->schemes[j].scheme.version == s.version) {
                    if (strcmp(spine->schemes[j].content_hash,
                               content_hash) == 0) {
                        /* identical re-registration: idempotent */
                        continue;
                    }
                    set_errf(err,
                             "scheme %s v%lld already registered with "
                             "different content; use a new version",
                             s.id, (long long)s.version);
                    odk_spine_scheme_release(&s);
                    return ODK_SPINE_ERR_STATE;
                }
            }
        }
        odk_spine_scheme_release(&s);
    }

    /* append (atomic activation: all-or-nothing) */
    for (i = 0; i < n; i++) {
        reg_scheme_entry entry;
        const odk_json_value *rec = (root->type == ODK_JSON_OBJECT)
                                        ? root
                                        : root->items[i];

        memset(&entry, 0, sizeof(entry));
        if (odk_spine_scheme_from_dom(rec, &entry.scheme, err) !=
            ODK_SPINE_OK) {
            return ODK_SPINE_ERR_SCHEMA; /* validated above; cannot fail */
        }
        memcpy(entry.content_hash, content_hash, sizeof(entry.content_hash));
        entry.source_name = strdup(source_name != NULL ? source_name : "");
        entry.seq = ++spine->seq;
        entry.active = 1;
        /* supersede older active versions of the same id */
        {
            size_t j;
            for (j = 0; j < spine->scheme_count; j++) {
                if (strcmp(spine->schemes[j].scheme.id, entry.scheme.id) ==
                    0) {
                    spine->schemes[j].active = 0;
                }
            }
        }
        if (spine->scheme_count == spine->scheme_cap) {
            size_t ncap = spine->scheme_cap == 0 ? 8 : spine->scheme_cap * 2;
            reg_scheme_entry *ng = realloc(spine->schemes,
                                           ncap * sizeof(*ng));
            if (ng == NULL) {
                odk_spine_scheme_release(&entry.scheme);
                free(entry.source_name);
                set_err(err, "out of memory");
                return ODK_SPINE_ERR_MEMORY;
            }
            spine->schemes = ng;
            spine->scheme_cap = ncap;
        }
        spine->schemes[spine->scheme_count++] = entry;
    }
    return ODK_SPINE_OK;
}

/* ------------------------------------------------------------------ */
/* Registration: decomposition templates                               */
/* ------------------------------------------------------------------ */

static odk_spine_status register_templates(odk_spine *spine,
                                           const odk_json_value *root,
                                           const char *content_hash,
                                           const char *source_name,
                                           odk_spine_error *err)
{
    size_t n, i;

    n = (root->type == ODK_JSON_OBJECT) ? 1 : root->item_count;
    for (i = 0; i < n; i++) {
        odk_spine_decomposition_template t;
        odk_spine_status st;
        const odk_json_value *rec = (root->type == ODK_JSON_OBJECT)
                                        ? root
                                        : root->items[i];

        odk_spine_decomposition_template_init(&t);
        st = odk_spine_decomposition_template_from_dom(rec, &t, err);
        if (st == ODK_SPINE_OK) {
            st = odk_spine_validate_template(&t, err);
        }
        if (st == ODK_SPINE_OK) {
            size_t j;
            for (j = 0; j < spine->template_count; j++) {
                if (strcmp(spine->templates[j].tmpl.id, t.id) == 0 &&
                    spine->templates[j].tmpl.version == t.version) {
                    if (strcmp(spine->templates[j].content_hash,
                               content_hash) == 0) {
                        continue;
                    }
                    set_errf(err,
                             "template %s v%lld already registered with "
                             "different content; use a new version",
                             t.id, (long long)t.version);
                    st = ODK_SPINE_ERR_STATE;
                }
            }
        }
        odk_spine_decomposition_template_release(&t);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    for (i = 0; i < n; i++) {
        reg_template_entry entry;
        const odk_json_value *rec = (root->type == ODK_JSON_OBJECT)
                                        ? root
                                        : root->items[i];

        memset(&entry, 0, sizeof(entry));
        if (odk_spine_decomposition_template_from_dom(rec, &entry.tmpl,
                                                      err) != ODK_SPINE_OK) {
            return ODK_SPINE_ERR_SCHEMA;
        }
        memcpy(entry.content_hash, content_hash, sizeof(entry.content_hash));
        entry.source_name = strdup(source_name != NULL ? source_name : "");
        entry.seq = ++spine->seq;
        entry.active = 1;
        {
            size_t j;
            for (j = 0; j < spine->template_count; j++) {
                if (strcmp(spine->templates[j].tmpl.id, entry.tmpl.id) == 0) {
                    spine->templates[j].active = 0;
                }
            }
        }
        if (spine->template_count == spine->template_cap) {
            size_t ncap = spine->template_cap == 0 ? 8
                                                   : spine->template_cap * 2;
            reg_template_entry *ng = realloc(spine->templates,
                                             ncap * sizeof(*ng));
            if (ng == NULL) {
                odk_spine_decomposition_template_release(&entry.tmpl);
                free(entry.source_name);
                set_err(err, "out of memory");
                return ODK_SPINE_ERR_MEMORY;
            }
            spine->templates = ng;
            spine->template_cap = ncap;
        }
        spine->templates[spine->template_count++] = entry;
    }
    return ODK_SPINE_OK;
}

/* ------------------------------------------------------------------ */
/* Registration: vocabularies                                          */
/* ------------------------------------------------------------------ */

static odk_spine_status register_vocabularies(odk_spine *spine,
                                              const odk_json_value *root,
                                              const char *content_hash,
                                              const char *source_name,
                                              odk_spine_error *err)
{
    size_t n, i;

    n = (root->type == ODK_JSON_OBJECT) ? 1 : root->item_count;
    for (i = 0; i < n; i++) {
        odk_spine_vocabulary v;
        odk_spine_status st;
        const odk_json_value *rec = (root->type == ODK_JSON_OBJECT)
                                        ? root
                                        : root->items[i];

        odk_spine_vocabulary_init(&v);
        st = odk_spine_vocabulary_from_dom(rec, &v, err);
        if (st == ODK_SPINE_OK) {
            st = odk_spine_validate_vocabulary(&v, err);
        }
        if (st == ODK_SPINE_OK) {
            size_t j;
            for (j = 0; j < spine->vocab_count; j++) {
                if (strcmp(spine->vocabularies[j].vocab.id, v.id) == 0 &&
                    spine->vocabularies[j].vocab.version == v.version) {
                    if (strcmp(spine->vocabularies[j].content_hash,
                               content_hash) == 0) {
                        continue;
                    }
                    set_errf(err,
                             "vocabulary %s v%lld already registered with "
                             "different content; use a new version",
                             v.id, (long long)v.version);
                    st = ODK_SPINE_ERR_STATE;
                }
            }
        }
        odk_spine_vocabulary_release(&v);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    for (i = 0; i < n; i++) {
        reg_vocab_entry entry;
        const odk_json_value *rec = (root->type == ODK_JSON_OBJECT)
                                        ? root
                                        : root->items[i];

        memset(&entry, 0, sizeof(entry));
        if (odk_spine_vocabulary_from_dom(rec, &entry.vocab, err) !=
            ODK_SPINE_OK) {
            return ODK_SPINE_ERR_SCHEMA;
        }
        memcpy(entry.content_hash, content_hash, sizeof(entry.content_hash));
        entry.source_name = strdup(source_name != NULL ? source_name : "");
        entry.seq = ++spine->seq;
        entry.active = 1;
        {
            size_t j;
            for (j = 0; j < spine->vocab_count; j++) {
                if (strcmp(spine->vocabularies[j].vocab.id,
                           entry.vocab.id) == 0) {
                    spine->vocabularies[j].active = 0;
                }
            }
        }
        if (spine->vocab_count == spine->vocab_cap) {
            size_t ncap = spine->vocab_cap == 0 ? 8 : spine->vocab_cap * 2;
            reg_vocab_entry *ng = realloc(spine->vocabularies,
                                          ncap * sizeof(*ng));
            if (ng == NULL) {
                odk_spine_vocabulary_release(&entry.vocab);
                free(entry.source_name);
                set_err(err, "out of memory");
                return ODK_SPINE_ERR_MEMORY;
            }
            spine->vocabularies = ng;
            spine->vocab_cap = ncap;
        }
        spine->vocabularies[spine->vocab_count++] = entry;
    }
    return ODK_SPINE_OK;
}

/* ------------------------------------------------------------------ */
/* Registration: corpora (opaque, syntactically validated)             */
/* ------------------------------------------------------------------ */

static odk_spine_status register_corpus(odk_spine *spine, const char *json,
                                        const char *content_hash,
                                        const char *source_name,
                                        odk_spine_error *err)
{
    /* A corpus is data: the spine validates syntax only (one JSON
     * document or JSONL, one document per non-empty line) and records
     * name + content hash. It never interprets corpus content. */
    odk_json_value *root;
    odk_error jerr;
    const char *p;
    int one_doc = 0;

    root = odk_json_parse(json, strlen(json), &jerr);
    if (root != NULL) {
        one_doc = 1;
        odk_json_free(root);
    }
    if (!one_doc) {
        /* JSONL? */
        p = json;
        while (*p != '\0') {
            const char *eol = strchr(p, '\n');
            size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);

            if (len > 0) {
                odk_json_value *line = odk_json_parse(p, len, &jerr);
                if (line == NULL) {
                    set_errf(err, "corpus line is not valid JSON: %s",
                             jerr.text);
                    return ODK_SPINE_ERR_SCHEMA;
                }
                odk_json_free(line);
            }
            if (eol == NULL) {
                break;
            }
            p = eol + 1;
        }
    }
    {
        reg_corpus_entry entry;

        memset(&entry, 0, sizeof(entry));
        memcpy(entry.content_hash, content_hash,
               sizeof(entry.content_hash));
        entry.source_name = strdup(source_name != NULL ? source_name : "");
        entry.seq = ++spine->seq;
        if (spine->corpus_count == spine->corpus_cap) {
            size_t ncap = spine->corpus_cap == 0 ? 8 : spine->corpus_cap * 2;
            reg_corpus_entry *ng = realloc(spine->corpora,
                                           ncap * sizeof(*ng));
            if (ng == NULL) {
                free(entry.source_name);
                set_err(err, "out of memory");
                return ODK_SPINE_ERR_MEMORY;
            }
            spine->corpora = ng;
            spine->corpus_cap = ncap;
        }
        spine->corpora[spine->corpus_count++] = entry;
    }
    return ODK_SPINE_OK;
}

/* ------------------------------------------------------------------ */
/* Registration: brain snapshots                                       */
/* ------------------------------------------------------------------ */

odk_spine_status odk_spine_register_brain(odk_spine *spine, odk_brain *brain,
                                          const char *snapshot_id,
                                          odk_spine_error *err)
{
    reg_brain_entry entry;
    char hash[17];

    if (spine == NULL || brain == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    if (!id_ok(snapshot_id)) {
        set_err(err, "brain snapshot: invalid snapshot id");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    {
        size_t i;

        for (i = 0; i < spine->brain_count; i++) {
            if (strcmp(spine->brains[i].snapshot_id, snapshot_id) == 0) {
                set_errf(err, "brain snapshot '%s' already registered",
                         snapshot_id);
                return ODK_SPINE_ERR_STATE;
            }
        }
    }
    /* Content identity: the application owns the snapshot file; the
     * registry hashes the snapshot id + epoch so pins stay comparable. */
    {
        char epoch_buf[32];
        uint64_t h;

        snprintf(epoch_buf, sizeof(epoch_buf), "%s:%zu", snapshot_id,
                 odk_brain_epoch(brain));
        odk_spine_fnv1a64_init(&h);
        odk_spine_fnv1a64_update(&h, epoch_buf, strlen(epoch_buf));
        odk_spine_fnv1a64_hex(h, hash);
    }
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.snapshot_id, sizeof(entry.snapshot_id), "%s", snapshot_id);
    memcpy(entry.content_hash, hash, sizeof(entry.content_hash));
    entry.brain = brain;
    entry.seq = ++spine->seq;
    if (spine->brain_count == spine->brain_cap) {
        size_t ncap = spine->brain_cap == 0 ? 4 : spine->brain_cap * 2;
        reg_brain_entry *ng = realloc(spine->brains, ncap * sizeof(*ng));
        if (ng == NULL) {
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
        spine->brains = ng;
        spine->brain_cap = ncap;
    }
    spine->brains[spine->brain_count++] = entry;
    return ODK_SPINE_OK;
}

/* ------------------------------------------------------------------ */
/* Public registration entry points                                    */
/* ------------------------------------------------------------------ */

odk_spine_status odk_spine_register_json(odk_spine *spine, const char *kind,
                                         const char *json,
                                         const char *source_name,
                                         odk_spine_error *err)
{
    odk_json_value *root;
    int is_array;
    odk_spine_status st;
    char hash[17];

    if (spine == NULL || kind == NULL || json == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    if (!kind_known(kind)) {
        set_errf(err, "unknown registration kind '%s'", kind);
        return ODK_SPINE_ERR_ARGUMENT;
    }
    if (source_name != NULL && !id_ok(source_name) &&
        strcmp(kind, "corpus") != 0) {
        set_err(err, "invalid source name");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    if (strcmp(kind, "brain_snapshot") == 0) {
        set_err(err,
                "brain snapshots are registered with odk_spine_register_brain");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_hash_hex(json, strlen(json), hash);
    /* corpora may be JSONL: one document per non-empty line */
    if (strcmp(kind, "corpus") == 0) {
        return register_corpus(spine, json, hash, source_name, err);
    }
    root = parse_records(json, &is_array, err);
    if (root == NULL) {
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (strcmp(kind, "scheme_set") == 0) {
        st = register_scheme_set(spine, root, hash, source_name, err);
    } else if (strcmp(kind, "decomposition_template") == 0) {
        st = register_templates(spine, root, hash, source_name, err);
    } else if (strcmp(kind, "vocabulary") == 0) {
        st = register_vocabularies(spine, root, hash, source_name, err);
    } else {
        st = register_corpus(spine, json, hash, source_name, err);
    }
    odk_json_free(root);
    return st;
}

odk_spine_status odk_spine_register_file(odk_spine *spine, const char *kind,
                                         const char *path,
                                         odk_spine_error *err)
{
    char *text;
    odk_spine_status st;

    if (path == NULL) {
        set_err(err, "null path");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    text = odk_spine_read_file(path, NULL);
    if (text == NULL) {
        set_errf(err, "cannot read '%s'", path);
        return ODK_SPINE_ERR_ARGUMENT;
    }
    st = odk_spine_register_json(spine, kind, text, path, err);
    free(text);
    return st;
}

odk_spine_status odk_spine_register_provider(odk_spine *spine,
                                             const char *kind,
                                             odk_spine_provider_fn provider,
                                             void *user,
                                             const char *source_name,
                                             odk_spine_error *err)
{
    char *text = NULL;
    odk_spine_status st;

    if (provider == NULL) {
        set_err(err, "null provider");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    if (provider(user, &text, err) != 0 || text == NULL) {
        set_err(err, "provider failed");
        return ODK_SPINE_ERR_STATE;
    }
    st = odk_spine_register_json(spine, kind, text, source_name, err);
    free(text);
    return st;
}

/* ------------------------------------------------------------------ */
/* Listing (deterministic: registration order, active entries only)    */
/* ------------------------------------------------------------------ */

void odk_spine_registration_ref_list_release(
    odk_spine_registration_ref_list *list)
{
    size_t i;

    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->count; i++) {
        free(list->items[i].kind);
        free(list->items[i].id);
        free(list->items[i].content_hash);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

odk_spine_status odk_spine_registrations(const odk_spine *spine,
                                         odk_spine_registration_ref_list *out,
                                         odk_spine_error *err)
{
    size_t total, i;
    uint64_t last_seq = 0;

    if (spine == NULL || out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    total = 0;
    for (i = 0; i < spine->scheme_count; i++) {
        if (spine->schemes[i].active) {
            total++;
        }
    }
    for (i = 0; i < spine->template_count; i++) {
        if (spine->templates[i].active) {
            total++;
        }
    }
    for (i = 0; i < spine->vocab_count; i++) {
        if (spine->vocabularies[i].active) {
            total++;
        }
    }
    total += spine->corpus_count + spine->brain_count;

    out->count = 0;
    out->items = NULL;
    if (total > 0) {
        out->items = calloc(total, sizeof(odk_spine_registration_ref));
        if (out->items == NULL) {
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    /* walk in registration order: pick the next-smallest seq each round */
    for (;;) {
        reg_scheme_entry *best_s = NULL;
        reg_template_entry *best_t = NULL;
        reg_vocab_entry *best_v = NULL;
        reg_corpus_entry *best_c = NULL;
        reg_brain_entry *best_b = NULL;
        uint64_t best = UINT64_MAX;

        for (i = 0; i < spine->scheme_count; i++) {
            if (spine->schemes[i].active && spine->schemes[i].seq > last_seq &&
                spine->schemes[i].seq < best) {
                best = spine->schemes[i].seq;
                best_s = &spine->schemes[i];
            }
        }
        for (i = 0; i < spine->template_count; i++) {
            if (spine->templates[i].active &&
                spine->templates[i].seq > last_seq &&
                spine->templates[i].seq < best) {
                best = spine->templates[i].seq;
                best_t = &spine->templates[i];
            }
        }
        for (i = 0; i < spine->vocab_count; i++) {
            if (spine->vocabularies[i].active &&
                spine->vocabularies[i].seq > last_seq &&
                spine->vocabularies[i].seq < best) {
                best = spine->vocabularies[i].seq;
                best_v = &spine->vocabularies[i];
            }
        }
        for (i = 0; i < spine->corpus_count; i++) {
            if (spine->corpora[i].seq > last_seq &&
                spine->corpora[i].seq < best) {
                best = spine->corpora[i].seq;
                best_c = &spine->corpora[i];
            }
        }
        for (i = 0; i < spine->brain_count; i++) {
            if (spine->brains[i].seq > last_seq &&
                spine->brains[i].seq < best) {
                best = spine->brains[i].seq;
                best_b = &spine->brains[i];
            }
        }
        if (best_s != NULL) {
            odk_spine_registration_ref *r = &out->items[out->count++];
            r->kind = strdup("scheme_set");
            r->id = strdup(best_s->scheme.id);
            r->version = best_s->scheme.version;
            r->content_hash = strdup(best_s->content_hash);
            last_seq = best_s->seq;
        } else if (best_t != NULL) {
            odk_spine_registration_ref *r = &out->items[out->count++];
            r->kind = strdup("decomposition_template");
            r->id = strdup(best_t->tmpl.id);
            r->version = best_t->tmpl.version;
            r->content_hash = strdup(best_t->content_hash);
            last_seq = best_t->seq;
        } else if (best_v != NULL) {
            odk_spine_registration_ref *r = &out->items[out->count++];
            r->kind = strdup("vocabulary");
            r->id = strdup(best_v->vocab.id);
            r->version = best_v->vocab.version;
            r->content_hash = strdup(best_v->content_hash);
            last_seq = best_v->seq;
        } else if (best_c != NULL) {
            odk_spine_registration_ref *r = &out->items[out->count++];
            r->kind = strdup("corpus");
            r->id = strdup(best_c->source_name);
            r->version = 1;
            r->content_hash = strdup(best_c->content_hash);
            last_seq = best_c->seq;
        } else if (best_b != NULL) {
            odk_spine_registration_ref *r = &out->items[out->count++];
            r->kind = strdup("brain_snapshot");
            r->id = strdup(best_b->snapshot_id);
            r->version = 1;
            r->content_hash = strdup(best_b->content_hash);
            last_seq = best_b->seq;
        } else {
            break;
        }
    }
    return ODK_SPINE_OK;
}

/* ------------------------------------------------------------------ */
/* As-of lookups                                                       */
/* ------------------------------------------------------------------ */

static int covers_time(int64_t valid_from, int64_t valid_to, int64_t t)
{
    return valid_from <= t && (valid_to == -1 || t <= valid_to);
}

odk_spine_status odk_spine_scheme_at(const odk_spine *spine, const char *id,
                                     int64_t version, int64_t t,
                                     const odk_spine_scheme **out,
                                     odk_spine_error *err)
{
    size_t i;
    const reg_scheme_entry *best = NULL;

    if (spine == NULL || id == NULL || out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    *out = NULL;
    for (i = 0; i < spine->scheme_count; i++) {
        const reg_scheme_entry *e = &spine->schemes[i];

        if (strcmp(e->scheme.id, id) != 0) {
            continue;
        }
        if (version > 0) {
            if (e->scheme.version == version) {
                *out = &e->scheme;
                return ODK_SPINE_OK;
            }
            continue;
        }
        /* as-of: newest valid_from wins; tie -> highest version */
        if (covers_time(e->scheme.valid_from, e->scheme.valid_to, t)) {
            if (best == NULL ||
                e->scheme.valid_from > best->scheme.valid_from ||
                (e->scheme.valid_from == best->scheme.valid_from &&
                 e->scheme.version > best->scheme.version)) {
                best = e;
            }
        }
    }
    if (*out == NULL && best == NULL) {
        set_errf(err, "scheme '%s' (v%lld) not valid at t=%lld", id,
                 (long long)version, (long long)t);
        return ODK_SPINE_ERR_NOT_FOUND;
    }
    if (*out == NULL) {
        *out = &best->scheme;
    }
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_template_at(
    const odk_spine *spine, const char *id, int64_t version, int64_t t,
    const odk_spine_decomposition_template **out, odk_spine_error *err)
{
    size_t i;
    const reg_template_entry *best = NULL;

    if (spine == NULL || id == NULL || out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    *out = NULL;
    for (i = 0; i < spine->template_count; i++) {
        const reg_template_entry *e = &spine->templates[i];

        if (strcmp(e->tmpl.id, id) != 0) {
            continue;
        }
        if (version > 0) {
            if (e->tmpl.version == version) {
                *out = &e->tmpl;
                return ODK_SPINE_OK;
            }
            continue;
        }
        if (covers_time(e->tmpl.valid_from, e->tmpl.valid_to, t)) {
            if (best == NULL || e->tmpl.valid_from > best->tmpl.valid_from ||
                (e->tmpl.valid_from == best->tmpl.valid_from &&
                 e->tmpl.version > best->tmpl.version)) {
                best = e;
            }
        }
    }
    if (*out == NULL && best == NULL) {
        set_errf(err, "template '%s' (v%lld) not valid at t=%lld", id,
                 (long long)version, (long long)t);
        return ODK_SPINE_ERR_NOT_FOUND;
    }
    if (*out == NULL) {
        *out = &best->tmpl;
    }
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_vocabulary_at(
    const odk_spine *spine, const char *id, int64_t version, int64_t t,
    const odk_spine_vocabulary **out, odk_spine_error *err)
{
    size_t i;
    const reg_vocab_entry *best = NULL;

    if (spine == NULL || id == NULL || out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    *out = NULL;
    for (i = 0; i < spine->vocab_count; i++) {
        const reg_vocab_entry *e = &spine->vocabularies[i];

        if (strcmp(e->vocab.id, id) != 0) {
            continue;
        }
        if (version > 0) {
            if (e->vocab.version == version) {
                *out = &e->vocab;
                return ODK_SPINE_OK;
            }
            continue;
        }
        if (covers_time(e->vocab.valid_from, e->vocab.valid_to, t)) {
            if (best == NULL || e->vocab.valid_from > best->vocab.valid_from ||
                (e->vocab.valid_from == best->vocab.valid_from &&
                 e->vocab.version > best->vocab.version)) {
                best = e;
            }
        }
    }
    if (*out == NULL && best == NULL) {
        set_errf(err, "vocabulary '%s' (v%lld) not valid at t=%lld", id,
                 (long long)version, (long long)t);
        return ODK_SPINE_ERR_NOT_FOUND;
    }
    if (*out == NULL) {
        *out = &best->vocab;
    }
    return ODK_SPINE_OK;
}

const odk_brain *odk_spine_brain_at(const odk_spine *spine,
                                    const char *snapshot_id)
{
    size_t i;

    if (spine == NULL || snapshot_id == NULL) {
        return NULL;
    }
    for (i = 0; i < spine->brain_count; i++) {
        if (strcmp(spine->brains[i].snapshot_id, snapshot_id) == 0) {
            return spine->brains[i].brain;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Registry teardown                                                   */
/* ------------------------------------------------------------------ */

void odk_spine_registry_free(struct odk_spine *spine)
{
    size_t i;

    for (i = 0; i < spine->scheme_count; i++) {
        odk_spine_scheme_release(&spine->schemes[i].scheme);
        free(spine->schemes[i].source_name);
    }
    free(spine->schemes);
    for (i = 0; i < spine->template_count; i++) {
        odk_spine_decomposition_template_release(&spine->templates[i].tmpl);
        free(spine->templates[i].source_name);
    }
    free(spine->templates);
    for (i = 0; i < spine->vocab_count; i++) {
        odk_spine_vocabulary_release(&spine->vocabularies[i].vocab);
        free(spine->vocabularies[i].source_name);
    }
    free(spine->vocabularies);
    for (i = 0; i < spine->corpus_count; i++) {
        free(spine->corpora[i].source_name);
    }
    free(spine->corpora);
    free(spine->brains); /* brains are application-owned */
    for (i = 0; i < spine->pin_count; i++) {
        odk_spine_decision_pin_release(&spine->pins[i].pin);
    }
    free(spine->pins);
    for (i = 0; i < spine->interval_count; i++) {
        odk_spine_classification_interval_release(
            &spine->intervals[i].interval);
    }
    free(spine->intervals);
    for (i = 0; i < spine->observation_count; i++) {
        odk_spine_observation_release(&spine->observations[i].obs);
    }
    free(spine->observations);
}
