#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
"""
gen_types.py - schema-first code generator for OmniDecide v2.0.0 (F5).

Reads schema/omnidecide.schema (the single source of truth) and emits:
  odk-spine/odk_spine_types.h   C record types + API prototypes
  odk-spine/odk_spine_types.c   JSON writers and strict parsers

The output is byte-deterministic: field order in the schema fixes C struct
order and JSON key order; no timestamps or environment data enter the
generated files. Regenerate with:  python3 tools/gen_types.py
"""

import json
import sys
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA_PATH = os.path.join(ROOT, "schema", "omnidecide.schema")
OUT_H = os.path.join(ROOT, "odk-spine", "odk_spine_types.h")
OUT_C = os.path.join(ROOT, "odk-spine", "odk_spine_types.c")

BANNER = """\
/* GENERATED FILE - DO NOT EDIT.
 *
 * Source of truth: schema/omnidecide.schema
 * Generator:       tools/gen_types.py
 * Schema version:  {version}
 * Schema fingerprint (FNV-1a 64 of the schema file bytes): {fp}
 *
 * Hand-maintained duplicates of these types are forbidden and CI-checked
 * (tools/check_schema_dup.sh). Regenerate with: python3 tools/gen_types.py
 */
"""

ALLOWED_FIELD_TYPES = {"string", "bool", "int64", "double",
                       "string[]", "map", "object<>[]", "object<>"}


def fnv1a64(data: bytes) -> int:
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def snake(name: str) -> str:
    s = re.sub(r'(?<!^)(?=[A-Z])', '_', name).lower()
    return s


def c_type_of(field_type: str, types_by_name: dict) -> str:
    if field_type == "string":
        return "char *"
    if field_type == "bool":
        return "int"
    if field_type == "int64":
        return "int64_t"
    if field_type == "double":
        return "double"
    if field_type == "string[]":
        return "odk_spine_str_list"
    if field_type == "map":
        return "odk_spine_map"
    m = re.match(r"^object<(\w+)>(\[\])?$", field_type)
    if m:
        ref = m.group(1)
        if ref not in types_by_name:
            raise SystemExit(f"schema error: unknown referenced type '{ref}'")
        return (f"odk_spine_{snake(ref)}_list" if m.group(2)
                else f"odk_spine_{snake(ref)}")
    raise SystemExit(f"schema error: bad field type '{field_type}'")


def validate(schema: dict) -> dict:
    if schema.get("schema") != "omnidecide":
        raise SystemExit("schema error: schema field must be 'omnidecide'")
    names = set()
    for t in schema["types"]:
        n = t["name"]
        if not re.match(r"^[A-Z][A-Za-z0-9]*$", n):
            raise SystemExit(f"schema error: bad type name '{n}'")
        if n in names:
            raise SystemExit(f"schema error: duplicate type '{n}'")
        names.add(n)
    types_by_name = {t["name"]: t for t in schema["types"]}
    for t in schema["types"]:
        fields = [f["name"] for f in t["fields"]]
        if len(fields) != len(set(fields)):
            raise SystemExit(f"schema error: duplicate field in {t['name']}")
        if not fields:
            raise SystemExit(f"schema error: {t['name']} has no fields")
        for f in t["fields"]:
            ft = f["type"]
            if not (ft in ALLOWED_FIELD_TYPES or
                    re.match(r"^object<\w+>(\[\])?$", ft)):
                raise SystemExit(
                    f"schema error: {t['name']}.{f['name']}: bad type '{ft}'")
            if ft.startswith("object<"):
                ref = re.match(r"^object<(\w+)>", ft).group(1)
                if ref not in types_by_name:
                    raise SystemExit(
                        f"schema error: {t['name']}.{f['name']}: "
                        f"unknown type '{ref}'")
            if "allowed_values" in f and ft != "string":
                raise SystemExit(
                    f"schema error: {t['name']}.{f['name']}: allowed_values "
                    "only on string fields")
            if not re.match(r"^[a-z][a-z0-9_]*$", f["name"]):
                raise SystemExit(
                    f"schema error: bad field name '{f['name']}'")
    return types_by_name


def gen_header(schema, types_by_name, fp):
    L = []
    L.append(BANNER.format(version=schema["version"], fp=fp))
    L.append("#ifndef ODK_SPINE_TYPES_H")
    L.append("#define ODK_SPINE_TYPES_H")
    L.append("")
    L.append("#ifndef ODK_SPINE_SCHEMA_FP_VALUE")
    L.append('#define ODK_SPINE_SCHEMA_FP_VALUE "%s"' % fp)
    L.append("#endif")
    L.append('#define ODK_SPINE_SCHEMA_VERSION "%s"' % schema["version"])
    L.append("")
    L.append('#include "odk_spine_base.h"')
    L.append("")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("#ifdef __cplusplus")
    L.append('extern "C" {')
    L.append("#endif")
    L.append("")
    L.append("/* Ordered string key/value pair (map entries keep JSON order). */")
    L.append("typedef struct {")
    L.append("    char *key;")
    L.append("    char *value;")
    L.append("} odk_spine_kv;")
    L.append("")
    L.append("/* Owned string array. */")
    L.append("typedef struct {")
    L.append("    size_t count;")
    L.append("    char **items;")
    L.append("} odk_spine_str_list;")
    L.append("")
    L.append("/* Ordered string map. */")
    L.append("typedef struct {")
    L.append("    size_t count;")
    L.append("    odk_spine_kv *items;")
    L.append("} odk_spine_map;")
    L.append("")
    for t in schema["types"]:
        c = snake(t["name"])
        L.append("/* %s */" % t.get("description", t["name"]).strip())
        L.append("typedef struct {")
        for f in t["fields"]:
            L.append("    %s %s;" % (c_type_of(f["type"], types_by_name),
                                     f["name"]))
        L.append("} odk_spine_%s;" % c)
        L.append("")
        L.append("/* Owned array of odk_spine_%s. */" % c)
        L.append("typedef struct {")
        L.append("    size_t count;")
        L.append("    odk_spine_%s *items;" % c)
        L.append("} odk_spine_%s_list;" % c)
        L.append("")
    L.append("/* ------------------------------------------------------------------ */")
    L.append("/* Generated record API. Memory model:                                */")
    L.append("/*   *_init      zero the record (empty lists, NULL strings).         */")
    L.append("/*   *_from_json strict parse: every schema field required, unknown    */")
    L.append("/*               fields rejected; allocates; on error the record is    */")
    L.append("/*               reset.                                                */")
    L.append("/*   *_to_json   compact JSON, keys in schema field order; returns a   */")
    L.append("/*               malloc'd string (caller frees) or NULL.               */")
    L.append("/*   *_release   free all owned memory, then zero the record.          */")
    L.append("/* ------------------------------------------------------------------ */")
    L.append("")
    for t in schema["types"]:
        c = snake(t["name"])
        L.append("void odk_spine_%s_init(odk_spine_%s *r);" % (c, c))
        L.append("void odk_spine_%s_release(odk_spine_%s *r);" % (c, c))
        L.append("char *odk_spine_%s_to_json(const odk_spine_%s *r);"
                 % (c, c))
        L.append("odk_spine_status odk_spine_%s_from_json(const char *text,"
                 % c)
        L.append("                                        odk_spine_%s *out,"
                 % c)
        L.append("                                        odk_spine_error *err);")
        L.append("odk_spine_status odk_spine_%s_from_dom(const odk_json_value *v,"
                 % c)
        L.append("                                       odk_spine_%s *out,"
                 % c)
        L.append("                                       odk_spine_error *err);")
        L.append("")
    L.append("#ifdef __cplusplus")
    L.append("}")
    L.append("#endif")
    L.append("")
    L.append("#endif /* ODK_SPINE_TYPES_H */")
    L.append("")
    return "\n".join(L)


def emit_init(t, L):
    L.append("void odk_spine_%s_init(odk_spine_%s *r)" % (c := snake(t["name"]), c))
    L.append("{")
    L.append("    if (r != NULL) {")
    L.append("        memset(r, 0, sizeof(*r));")
    L.append("    }")
    L.append("}")
    L.append("")


def emit_release(t, types_by_name, L):
    c = snake(t["name"])
    needs_i = any(f["type"] in ("string[]", "map") or
                  f["type"].endswith(">[]") for f in t["fields"])
    L.append("void odk_spine_%s_release(odk_spine_%s *r)" % (c, c))
    L.append("{")
    if needs_i:
        L.append("    size_t i;")
    L.append("    if (r == NULL) {")
    L.append("        return;")
    L.append("    }")
    for f in t["fields"]:
        ft = f["type"]
        if ft == "string":
            L.append("    free(r->%s);" % f["name"])
        elif ft == "string[]":
            L.append("    for (i = 0; i < r->%s.count; i++) {" % f["name"])
            L.append("        free(r->%s.items[i]);" % f["name"])
            L.append("    }")
            L.append("    free(r->%s.items);" % f["name"])
        elif ft == "map":
            L.append("    for (i = 0; i < r->%s.count; i++) {" % f["name"])
            L.append("        free(r->%s.items[i].key);" % f["name"])
            L.append("        free(r->%s.items[i].value);" % f["name"])
            L.append("    }")
            L.append("    free(r->%s.items);" % f["name"])
        elif ft.endswith(">[]"):
            ref = re.match(r"^object<(\w+)>", ft).group(1)
            L.append("    for (i = 0; i < r->%s.count; i++) {" % f["name"])
            L.append("        odk_spine_%s_release(&r->%s.items[i]);"
                     % (snake(ref), f["name"]))
            L.append("    }")
            L.append("    free(r->%s.items);" % f["name"])
        elif ft.startswith("object<"):
            ref = re.match(r"^object<(\w+)>", ft).group(1)
            L.append("    odk_spine_%s_release(&r->%s);"
                     % (snake(ref), f["name"]))
    L.append("    memset(r, 0, sizeof(*r));")
    L.append("}")
    L.append("")


def emit_to_json(t, types_by_name, L):
    c = snake(t["name"])
    L.append("char *odk_spine_%s_to_json(const odk_spine_%s *r)" % (c, c))
    L.append("{")
    L.append("    gbuf b;")
    L.append("")
    L.append("    if (r == NULL || gbuf_init(&b) != 0) {")
    L.append("        return NULL;")
    L.append("    }")
    parts = []
    for idx, f in enumerate(t["fields"]):
        ft = f["type"]
        key = '{"%s":' % f["name"]
        raw = key if idx == 0 else ',"%s":' % f["name"]
        # escape for embedding in a C string literal
        cliteral = raw.replace('\\', '\\\\').replace('"', '\\"')
        prefix = "    if (gbuf_puts(&b, \"%s\") != 0 ||\n" % cliteral
        body = []
        if ft == "string":
            body.append("        gbuf_put_json_string(&b, r->%s) != 0"
                        % f["name"])
        elif ft == "bool":
            body.append("        gbuf_put_bool(&b, r->%s) != 0" % f["name"])
        elif ft == "int64":
            body.append("        gbuf_put_int(&b, r->%s) != 0" % f["name"])
        elif ft == "double":
            body.append("        gbuf_put_double(&b, r->%s) != 0" % f["name"])
        elif ft == "string[]":
            body.append("        gbuf_put_str_list(&b, &r->%s) != 0"
                        % f["name"])
        elif ft == "map":
            body.append("        gbuf_put_map(&b, &r->%s) != 0" % f["name"])
        elif ft.endswith(">[]"):
            ref = re.match(r"^object<(\w+)>", ft).group(1)
            body.append("        gbuf_putc(&b, '[') != 0 ||\n"
                        "        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)"
                        "odk_spine_%s_to_json,\n"
                        "                           r->%s.items,\n"
                        "                           r->%s.count,\n"
                        "                           sizeof(odk_spine_%s)) != 0 ||\n"
                        "        gbuf_putc(&b, ']') != 0"
                        % (snake(ref), f["name"], f["name"], snake(ref)))
        elif ft.startswith("object<"):
            ref = re.match(r"^object<(\w+)>", ft).group(1)
            body.append("        gbuf_put_embedded(&b, (gbuf_obj_to_json)"
                        "odk_spine_%s_to_json,\n"
                        "                           &r->%s) != 0"
                        % (snake(ref), f["name"]))
        L.append(prefix + " ||\n".join(body) + ") {")
        L.append("        gbuf_free(&b);")
        L.append("        return NULL;")
        L.append("    }")
    L.append("    if (gbuf_putc(&b, '}') != 0) {")
    L.append("        gbuf_free(&b);")
    L.append("        return NULL;")
    L.append("    }")
    L.append("    return gbuf_detach(&b);")
    L.append("}")
    L.append("")


def emit_from_dom(t, types_by_name, L):
    c = snake(t["name"])
    T = t["name"]
    L.append("odk_spine_status odk_spine_%s_from_dom(const odk_json_value *v,"
             % c)
    L.append("                                       odk_spine_%s *r,"
             % c)
    L.append("                                       odk_spine_error *err)")
    L.append("{")
    L.append("    static const char *const g_fields[] = {")
    for f in t["fields"]:
        L.append('        "%s",' % f["name"])
    L.append("        NULL")
    L.append("    };")
    L.append("    const odk_json_value *fv;")
    L.append("    size_t i;")
    L.append("    size_t k;")
    L.append("")
    L.append("    if (v == NULL || v->type != ODK_JSON_OBJECT) {")
    L.append('        gerr(err, "%s: expected an object");' % T)
    L.append("        return ODK_SPINE_ERR_SCHEMA;")
    L.append("    }")
    L.append("    for (k = 0; k < v->member_count; k++) {")
    L.append("        int known = 0;")
    L.append("        for (i = 0; g_fields[i] != NULL; i++) {")
    L.append("            if (strcmp(v->keys[k], g_fields[i]) == 0) {")
    L.append("                known = 1;")
    L.append("                break;")
    L.append("            }")
    L.append("        }")
    L.append("        if (!known) {")
    L.append('            gerr(err, "%s: unknown field \'%s\'", v->keys[k]);'
             % (T, "%s"))
    L.append("            return ODK_SPINE_ERR_SCHEMA;")
    L.append("        }")
    L.append("    }")
    for f in t["fields"]:
        ft = f["type"]
        fn = f["name"]
        L.append("    fv = odk_json_object_get(v, \"%s\");" % fn)
        L.append("    if (fv == NULL) {")
        L.append('        gerr(err, "%s.%s: missing required field");' % (T, fn))
        L.append("        return ODK_SPINE_ERR_SCHEMA;")
        L.append("    }")
        if ft == "string":
            if "allowed_values" in f:
                vals = f["allowed_values"]
                L.append("    if (fv->type != ODK_JSON_STRING) {")
                L.append('        gerr(err, "%s.%s: expected a string");' % (T, fn))
                L.append("        return ODK_SPINE_ERR_SCHEMA;")
                L.append("    }")
                L.append("    {")
                L.append("        int ok_av = 0;")
                L.append("        static const char *const g_av[] = {")
                for av in vals:
                    L.append('            "%s",' % av)
                L.append("            NULL")
                L.append("        };")
                L.append("        for (i = 0; g_av[i] != NULL; i++) {")
                L.append("            if (strcmp(fv->string, g_av[i]) == 0) {")
                L.append("                ok_av = 1;")
                L.append("                break;")
                L.append("            }")
                L.append("        }")
                L.append("        if (!ok_av) {")
                L.append('            gerr(err, "%s.%s: value not allowed");' % (T, fn))
                L.append("            return ODK_SPINE_ERR_SCHEMA;")
                L.append("        }")
                L.append("    }")
            L.append("    r->%s = g_strdup(fv->string);" % fn)
            L.append("    if (r->%s == NULL) {" % fn)
            L.append("        gerr(err, \"out of memory\");")
            L.append("        return ODK_SPINE_ERR_MEMORY;")
            L.append("    }")
        elif ft == "bool":
            L.append("    if (fv->type != ODK_JSON_BOOL) {")
            L.append('        gerr(err, "%s.%s: expected true or false");' % (T, fn))
            L.append("        return ODK_SPINE_ERR_SCHEMA;")
            L.append("    }")
            L.append("    r->%s = fv->boolean;" % fn)
        elif ft == "int64":
            L.append("    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {")
            L.append('        gerr(err, "%s.%s: expected an integer");' % (T, fn))
            L.append("        return ODK_SPINE_ERR_SCHEMA;")
            L.append("    }")
            L.append("    r->%s = fv->integer;" % fn)
        elif ft == "double":
            L.append("    if (fv->type != ODK_JSON_NUMBER) {")
            L.append('        gerr(err, "%s.%s: expected a number");' % (T, fn))
            L.append("        return ODK_SPINE_ERR_SCHEMA;")
            L.append("    }")
            L.append("    r->%s = fv->number;" % fn)
        elif ft == "string[]":
            L.append("    if (fv->type != ODK_JSON_ARRAY) {")
            L.append('        gerr(err, "%s.%s: expected an array of strings");' % (T, fn))
            L.append("        return ODK_SPINE_ERR_SCHEMA;")
            L.append("    }")
            L.append("    if (fv->item_count > 0) {")
            L.append("        r->%s.items = calloc(fv->item_count, sizeof(char *));" % fn)
            L.append("        if (r->%s.items == NULL) {" % fn)
            L.append("            gerr(err, \"out of memory\");")
            L.append("            return ODK_SPINE_ERR_MEMORY;")
            L.append("        }")
            L.append("    }")
            L.append("    for (i = 0; i < fv->item_count; i++) {")
            L.append("        if (fv->items[i]->type != ODK_JSON_STRING) {")
            L.append('            gerr(err, "%s.%s[%%zu]: expected a string", i);' % (T, fn))
            L.append("            return ODK_SPINE_ERR_SCHEMA;")
            L.append("        }")
            L.append("        r->%s.items[i] = g_strdup(fv->items[i]->string);" % fn)
            L.append("        if (r->%s.items[i] == NULL) {" % fn)
            L.append("            gerr(err, \"out of memory\");")
            L.append("            return ODK_SPINE_ERR_MEMORY;")
            L.append("        }")
            L.append("    }")
            L.append("    r->%s.count = fv->item_count;" % fn)
        elif ft == "map":
            L.append("    if (fv->type != ODK_JSON_OBJECT) {")
            L.append('        gerr(err, "%s.%s: expected an object");' % (T, fn))
            L.append("        return ODK_SPINE_ERR_SCHEMA;")
            L.append("    }")
            L.append("    if (fv->member_count > 0) {")
            L.append("        r->%s.items = calloc(fv->member_count, sizeof(odk_spine_kv));" % fn)
            L.append("        if (r->%s.items == NULL) {" % fn)
            L.append("            gerr(err, \"out of memory\");")
            L.append("            return ODK_SPINE_ERR_MEMORY;")
            L.append("        }")
            L.append("    }")
            L.append("    for (i = 0; i < fv->member_count; i++) {")
            L.append("        if (fv->members[i]->type != ODK_JSON_STRING) {")
            L.append('            gerr(err, "%s.%s: values must be strings");' % (T, fn))
            L.append("            return ODK_SPINE_ERR_SCHEMA;")
            L.append("        }")
            L.append("        r->%s.items[i].key = g_strdup(fv->keys[i]);" % fn)
            L.append("        r->%s.items[i].value = g_strdup(fv->members[i]->string);" % fn)
            L.append("        if (r->%s.items[i].key == NULL ||" % fn)
            L.append("            r->%s.items[i].value == NULL) {" % fn)
            L.append("            gerr(err, \"out of memory\");")
            L.append("            return ODK_SPINE_ERR_MEMORY;")
            L.append("        }")
            L.append("    }")
            L.append("    r->%s.count = fv->member_count;" % fn)
        elif ft.endswith(">[]"):
            ref = re.match(r"^object<(\w+)>", ft).group(1)
            rc = snake(ref)
            L.append("    if (fv->type != ODK_JSON_ARRAY) {")
            L.append('        gerr(err, "%s.%s: expected an array");' % (T, fn))
            L.append("        return ODK_SPINE_ERR_SCHEMA;")
            L.append("    }")
            L.append("    if (fv->item_count > 0) {")
            L.append("        r->%s.items = calloc(fv->item_count, sizeof(odk_spine_%s));" % (fn, rc))
            L.append("        if (r->%s.items == NULL) {" % fn)
            L.append("            gerr(err, \"out of memory\");")
            L.append("            return ODK_SPINE_ERR_MEMORY;")
            L.append("        }")
            L.append("    }")
            L.append("    for (i = 0; i < fv->item_count; i++) {")
            L.append("        odk_spine_status st = odk_spine_%s_from_dom(fv->items[i],"
                     % rc)
            L.append("                                    &r->%s.items[i], err);" % fn)
            L.append("        if (st != ODK_SPINE_OK) {")
            L.append("            return st;")
            L.append("        }")
            L.append("    }")
            L.append("    r->%s.count = fv->item_count;" % fn)
        elif ft.startswith("object<"):
            ref = re.match(r"^object<(\w+)>", ft).group(1)
            rc = snake(ref)
            L.append("    {")
            L.append("        odk_spine_status st = odk_spine_%s_from_dom(fv, &r->%s, err);"
                     % (rc, fn))
            L.append("        if (st != ODK_SPINE_OK) {")
            L.append("            return st;")
            L.append("        }")
            L.append("    }")
    L.append("    return ODK_SPINE_OK;")
    L.append("}")
    L.append("")


def emit_from_json(t, L):
    c = snake(t["name"])
    T = t["name"]
    L.append("odk_spine_status odk_spine_%s_from_json(const char *text," % c)
    L.append("                                        odk_spine_%s *out," % c)
    L.append("                                        odk_spine_error *err)")
    L.append("{")
    L.append("    odk_json_value *root;")
    L.append("    odk_error jerr;")
    L.append("    odk_spine_status st;")
    L.append("")
    L.append("    if (text == NULL || out == NULL) {")
    L.append('        gerr(err, "%s: null argument");' % T)
    L.append("        return ODK_SPINE_ERR_ARGUMENT;")
    L.append("    }")
    L.append("    odk_spine_%s_init(out);" % c)
    L.append("    root = odk_json_parse(text, strlen(text), &jerr);")
    L.append("    if (root == NULL) {")
    L.append('        gerr(err, "%s: invalid JSON: %s", jerr.text);' % (T, "%s"))
    L.append("        return ODK_SPINE_ERR_SCHEMA;")
    L.append("    }")
    L.append("    st = odk_spine_%s_from_dom(root, out, err);" % c)
    L.append("    odk_json_free(root);")
    L.append("    if (st != ODK_SPINE_OK) {")
    L.append("        odk_spine_%s_release(out);" % c)
    L.append("        return st;")
    L.append("    }")
    L.append("    return ODK_SPINE_OK;")
    L.append("}")
    L.append("")


HELPERS = r"""
/* ------------------------------------------------------------------ */
/* Generated helpers (private to this file)                            */
/* ------------------------------------------------------------------ */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__GNUC__)
#define G_UNUSED __attribute__((unused))
#else
#define G_UNUSED
#endif

static void G_UNUSED gerr(odk_spine_error *err, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err->text, sizeof(err->text), fmt, ap);
    va_end(ap);
}

static char *G_UNUSED g_strdup(const char *s)
{
    size_t n;
    char *out;

    if (s == NULL) {
        return NULL;
    }
    n = strlen(s) + 1;
    out = malloc(n);
    if (out != NULL) {
        memcpy(out, s, n);
    }
    return out;
}

/* Growable output buffer (byte-deterministic; no locale, no wall clock). */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int oom;
} gbuf;

typedef char *(*gbuf_obj_to_json)(const void *record);

static int G_UNUSED gbuf_init(gbuf *b)
{
    b->cap = 256;
    b->len = 0;
    b->oom = 0;
    b->data = malloc(b->cap);
    return b->data == NULL ? -1 : 0;
}

static void G_UNUSED gbuf_free(gbuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static char *G_UNUSED gbuf_detach(gbuf *b)
{
    char *out = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return out;
}

static int G_UNUSED gbuf_reserve(gbuf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    char *nd;

    if (b->oom) {
        return -1;
    }
    if (need <= b->cap) {
        return 0;
    }
    while (b->cap < need) {
        b->cap *= 2;
    }
    nd = realloc(b->data, b->cap);
    if (nd == NULL) {
        b->oom = 1;
        return -1;
    }
    b->data = nd;
    return 0;
}

static int G_UNUSED gbuf_putc(gbuf *b, char c)
{
    if (gbuf_reserve(b, 1) != 0) {
        return -1;
    }
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return 0;
}

static int G_UNUSED gbuf_puts(gbuf *b, const char *s)
{
    size_t n = strlen(s);
    if (gbuf_reserve(b, n) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int G_UNUSED gbuf_put_json_string(gbuf *b, const char *s)
{
    char *lit;
    int rc;

    if (s == NULL) {
        s = "";
    }
    lit = odk_json_string_literal(s, strlen(s));
    if (lit == NULL) {
        return -1;
    }
    rc = gbuf_puts(b, lit);
    free(lit);
    return rc;
}

static int G_UNUSED gbuf_put_bool(gbuf *b, int v)
{
    return gbuf_puts(b, v ? "true" : "false");
}

static int G_UNUSED gbuf_put_int(gbuf *b, int64_t v)
{
    char *lit;
    int rc;

    lit = odk_json_int_literal((long long)v);
    if (lit == NULL) {
        return -1;
    }
    rc = gbuf_puts(b, lit);
    free(lit);
    return rc;
}

static int G_UNUSED gbuf_put_double(gbuf *b, double v)
{
    char *lit;
    int rc;

    if (!isfinite(v)) {
        return -1; /* schema forbids NaN/infinity in records */
    }
    lit = odk_json_number_literal(v);
    if (lit == NULL) {
        return -1;
    }
    rc = gbuf_puts(b, lit);
    free(lit);
    return rc;
}

static int G_UNUSED gbuf_put_str_list(gbuf *b, const odk_spine_str_list *l)
{
    size_t i;

    if (gbuf_putc(b, '[') != 0) {
        return -1;
    }
    for (i = 0; i < l->count; i++) {
        if (i > 0 && gbuf_putc(b, ',') != 0) {
            return -1;
        }
        if (gbuf_put_json_string(b, l->items[i]) != 0) {
            return -1;
        }
    }
    return gbuf_putc(b, ']');
}

static int G_UNUSED gbuf_put_map(gbuf *b, const odk_spine_map *m)
{
    size_t i;

    if (gbuf_putc(b, '{') != 0) {
        return -1;
    }
    for (i = 0; i < m->count; i++) {
        if (i > 0 && gbuf_putc(b, ',') != 0) {
            return -1;
        }
        if (gbuf_put_json_string(b, m->items[i].key) != 0 ||
            gbuf_putc(b, ':') != 0 ||
            gbuf_put_json_string(b, m->items[i].value) != 0) {
            return -1;
        }
    }
    return gbuf_putc(b, '}');
}

static int G_UNUSED gbuf_put_obj_list_s(gbuf *b, gbuf_obj_to_json to_json,
                               const void *items, size_t count,
                               size_t elem_size)
{
    size_t i;

    /* items only; the caller writes the surrounding brackets */
    for (i = 0; i < count; i++) {
        char *lit;
        int rc;

        if (i > 0 && gbuf_putc(b, ',') != 0) {
            return -1;
        }
        lit = to_json((const char *)items + i * elem_size);
        if (lit == NULL) {
            return -1;
        }
        rc = gbuf_puts(b, lit);
        free(lit);
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

static int G_UNUSED gbuf_put_embedded(gbuf *b, gbuf_obj_to_json to_json,
                             const void *record)
{
    char *lit;
    int rc;

    lit = to_json(record);
    if (lit == NULL) {
        return -1;
    }
    rc = gbuf_puts(b, lit);
    free(lit);
    return rc;
}
"""


def main():
    with open(SCHEMA_PATH, "rb") as f:
        raw = f.read()
    fp = "%016x" % fnv1a64(raw)
    if "--fingerprint" in sys.argv:
        print(fp)
        return
    schema = json.loads(raw.decode("utf-8"))
    types_by_name = validate(schema)

    h = gen_header(schema, types_by_name, fp)

    L = []
    L.append(BANNER.format(version=schema["version"], fp=fp))
    L.append('#include "odk_spine_types.h"')
    L.append(HELPERS)
    L2 = ["\n".join(L), ""]
    for t in schema["types"]:
        emit_init(t, L2)
        emit_release(t, types_by_name, L2)
        emit_to_json(t, types_by_name, L2)
        emit_from_dom(t, types_by_name, L2)
        emit_from_json(t, L2)
    L2.append("")
    c = "\n".join(L2)

    with open(OUT_H, "w") as f:
        f.write(h)
    with open(OUT_C, "w") as f:
        f.write(c)
    print("gen_types: schema %s, fingerprint %s" % (schema["version"], fp))
    print("gen_types: wrote %s (%d types)" % (OUT_H, len(schema["types"])))
    print("gen_types: wrote %s" % OUT_C)


if __name__ == "__main__":
    main()
