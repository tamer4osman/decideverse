/* GENERATED FILE - DO NOT EDIT.
 *
 * Source of truth: schema/omnidecide.schema
 * Generator:       tools/gen_types.py
 * Schema version:  1.0.0
 * Schema fingerprint (FNV-1a 64 of the schema file bytes): 88ffaefc9143dcc0
 *
 * Hand-maintained duplicates of these types are forbidden and CI-checked
 * (tools/check_schema_dup.sh). Regenerate with: python3 tools/gen_types.py
 */

#ifndef ODK_SPINE_TYPES_H
#define ODK_SPINE_TYPES_H

#ifndef ODK_SPINE_SCHEMA_FP_VALUE
#define ODK_SPINE_SCHEMA_FP_VALUE "88ffaefc9143dcc0"
#endif
#define ODK_SPINE_SCHEMA_VERSION "1.0.0"

#include "odk_spine_base.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ordered string key/value pair (map entries keep JSON order). */
typedef struct {
    char *key;
    char *value;
} odk_spine_kv;

/* Owned string array. */
typedef struct {
    size_t count;
    char **items;
} odk_spine_str_list;

/* Ordered string map. */
typedef struct {
    size_t count;
    odk_spine_kv *items;
} odk_spine_map;

/* Reference to one registered customization (name + content hash) as recorded in decision pins and registry listings. */
typedef struct {
    char * kind;
    char * id;
    int64_t version;
    char * content_hash;
} odk_spine_registration_ref;

/* Owned array of odk_spine_registration_ref. */
typedef struct {
    size_t count;
    odk_spine_registration_ref *items;
} odk_spine_registration_ref_list;

/* The universal append-only record of the storage spine. Decisions, classification events and outcomes are all Observations; classification history is therefore queryable like any other variable. */
typedef struct {
    char * observation_id;
    char * kind;
    int64_t t;
    char * scope;
    char * subject_ref;
    char * primitive;
    char * scheme_id;
    char * payload_json;
    double confidence;
    char * tier;
    char * brain_snapshot_id;
    char * outcome;
    char * kernel_version;
    char * schema_fp;
    odk_spine_registration_ref_list registrations;
} odk_spine_observation;

/* Owned array of odk_spine_observation. */
typedef struct {
    size_t count;
    odk_spine_observation *items;
} odk_spine_observation_list;

/* One declarative matching rule of a scheme. Rules are evaluated in declaration order; among the rules whose condition holds, the highest priority wins, and ties are broken by the earlier declaration. */
typedef struct {
    char * label;
    odk_spine_str_list match_terms;
    int any_term;
    int64_t priority;
    odk_spine_map facets;
} odk_spine_scheme_rule;

/* Owned array of odk_spine_scheme_rule. */
typedef struct {
    size_t count;
    odk_spine_scheme_rule *items;
} odk_spine_scheme_rule_list;

/* A declarative, versioned classification scheme (F1). Schemes are data, never code; they are supplied by consuming applications through the F7 extension API. */
typedef struct {
    char * id;
    int64_t version;
    int64_t valid_from;
    int64_t valid_to;
    char * default_label;
    odk_spine_str_list facet_names;
    odk_spine_scheme_rule_list rules;
    char * notes;
} odk_spine_scheme;

/* Owned array of odk_spine_scheme. */
typedef struct {
    size_t count;
    odk_spine_scheme *items;
} odk_spine_scheme_list;

/* One canonical term with its accepted paraphrases. Data for Tier B classification resilience. */
typedef struct {
    char * canonical;
    odk_spine_str_list terms;
} odk_spine_synonym_group;

/* Owned array of odk_spine_synonym_group. */
typedef struct {
    size_t count;
    odk_spine_synonym_group *items;
} odk_spine_synonym_group_list;

/* A versioned, temporally valid synonym dictionary (F7 data). Tier B uses it to expand item and rule terms before matching. */
typedef struct {
    char * id;
    int64_t version;
    int64_t valid_from;
    int64_t valid_to;
    odk_spine_synonym_group_list groups;
} odk_spine_vocabulary;

/* Owned array of odk_spine_vocabulary. */
typedef struct {
    size_t count;
    odk_spine_vocabulary *items;
} odk_spine_vocabulary_list;

/* One scheme's assignment for one item (F1). Tier-labeled and, for Tier B, attributed to the brain snapshot that produced it. */
typedef struct {
    char * scheme_id;
    int64_t scheme_version;
    char * label;
    odk_spine_map facets;
    char * tier;
    char * brain_snapshot_id;
    double confidence;
    odk_spine_str_list matched_terms;
    int64_t rule_index;
} odk_spine_classification;

/* Owned array of odk_spine_classification. */
typedef struct {
    size_t count;
    odk_spine_classification *items;
} odk_spine_classification_list;

/* The one-call, N-schemes result document of odk_classify (F1): one Classification per active scheme version, grouped per scheme. */
typedef struct {
    char * subject_ref;
    int64_t t;
    odk_spine_classification_list classifications;
} odk_spine_classification_result;

/* Owned array of odk_spine_classification_result. */
typedef struct {
    size_t count;
    odk_spine_classification_result *items;
} odk_spine_classification_result_list;

/* One append-only validity interval of an item's classification (F2). Reclassification appends; history is first-class data. */
typedef struct {
    char * subject_ref;
    char * scheme_id;
    int64_t scheme_version;
    char * label;
    odk_spine_map facets;
    char * tier;
    char * brain_snapshot_id;
    int64_t valid_from;
    int64_t valid_to;
    char * observation_id;
} odk_spine_classification_interval;

/* Owned array of odk_spine_classification_interval. */
typedef struct {
    size_t count;
    odk_spine_classification_interval *items;
} odk_spine_classification_interval_list;

/* A declared parameter of a decomposition template. */
typedef struct {
    char * name;
    char * type;
    char * default_value;
    int required;
} odk_spine_template_parameter;

/* Owned array of odk_spine_template_parameter. */
typedef struct {
    size_t count;
    odk_spine_template_parameter *items;
} odk_spine_template_parameter_list;

/* Declares one component of a decomposition output. */
typedef struct {
    char * name;
    char * kind;
    int64_t max_items;
    int required;
} odk_spine_component_spec;

/* Owned array of odk_spine_component_spec. */
typedef struct {
    size_t count;
    odk_spine_component_spec *items;
} odk_spine_component_spec_list;

/* How one component is extracted from the item text. Deterministic modes: sentence (whole sentences containing any term, in text order), before/after/around (window chars around the first match), keyvalue (lines whose key matches a term). */
typedef struct {
    char * component;
    odk_spine_str_list terms;
    char * mode;
    int64_t window;
} odk_spine_extraction_rule;

/* Owned array of odk_spine_extraction_rule. */
typedef struct {
    size_t count;
    odk_spine_extraction_rule *items;
} odk_spine_extraction_rule_list;

/* A declarative, versioned decomposition contract (F3): parameters, component specs and extraction rules. The output components are the contract between the perception layer and the decision layer. */
typedef struct {
    char * id;
    int64_t version;
    int64_t valid_from;
    int64_t valid_to;
    odk_spine_template_parameter_list parameters;
    odk_spine_component_spec_list component_specs;
    odk_spine_extraction_rule_list extraction_rules;
} odk_spine_decomposition_template;

/* Owned array of odk_spine_decomposition_template. */
typedef struct {
    size_t count;
    odk_spine_decomposition_template *items;
} odk_spine_decomposition_template_list;

/* One extracted component of a decomposition output. */
typedef struct {
    char * name;
    char * kind;
    char * content;
    int64_t item_count;
    int complete;
} odk_spine_component;

/* Owned array of odk_spine_component. */
typedef struct {
    size_t count;
    odk_spine_component *items;
} odk_spine_component_list;

/* The decomposition output document (F3): resolved parameters plus the extracted components, in component-spec order. */
typedef struct {
    char * template_id;
    int64_t template_version;
    odk_spine_map params_used;
    odk_spine_component_list components;
} odk_spine_components;

/* Owned array of odk_spine_components. */
typedef struct {
    size_t count;
    odk_spine_components *items;
} odk_spine_components_list;

/* Append-only record of one decision (F6), sufficient for total replay: the exact request bytes ride along (request_json), the registered customization state is recorded by name and content hash, and the schema fingerprint keeps historical pins interpretable across schema evolution. */
typedef struct {
    char * pin_id;
    char * schema_fp;
    char * kernel_version;
    char * brain_snapshot_id;
    char * context_hash;
    char * inputs_ref;
    char * request_json;
    char * decision;
    double confidence;
    char * tier;
    char * replay_class;
    int64_t timestamp;
    odk_spine_registration_ref_list registrations;
} odk_spine_decision_pin;

/* Owned array of odk_spine_decision_pin. */
typedef struct {
    size_t count;
    odk_spine_decision_pin *items;
} odk_spine_decision_pin_list;

/* One reliability row of a calibration report (F4): predicted confidence vs observed outcome rate for one primitive/scheme/tier cell and one confidence bin. */
typedef struct {
    char * primitive;
    char * scheme_id;
    char * tier;
    int64_t bin_index;
    double lo;
    double hi;
    int64_t sample_count;
    double predicted_mean;
    double observed_rate;
    double calibration_error;
} odk_spine_calibration_bin;

/* Owned array of odk_spine_calibration_bin. */
typedef struct {
    size_t count;
    odk_spine_calibration_bin *items;
} odk_spine_calibration_bin_list;

/* Deterministic calibration report (F4): two runs on the same decision log produce byte-identical reports. Rows are ordered by primitive, scheme_id, tier, bin_index. */
typedef struct {
    char * report_id;
    char * generated_from;
    odk_spine_calibration_bin_list bins;
} odk_spine_calibration_report;

/* Owned array of odk_spine_calibration_report. */
typedef struct {
    size_t count;
    odk_spine_calibration_report *items;
} odk_spine_calibration_report_list;

/* One rendered variable of a worldState context. */
typedef struct {
    char * name;
    char * type;
    char * value;
} odk_spine_world_variable;

/* Owned array of odk_spine_world_variable. */
typedef struct {
    size_t count;
    odk_spine_world_variable *items;
} odk_spine_world_variable_list;

/* The worldState(variables, scope, t) contract shape (F6): the rendered context the kernel expects from the surrounding stack. scope is opaque and application-defined; the kernel consumes only the rendered result and never the selector's internals. The spine validates the shape; it does not implement storage. */
typedef struct {
    int64_t t;
    char * scope_json;
    odk_spine_world_variable_list variables;
} odk_spine_world_state_context;

/* Owned array of odk_spine_world_state_context. */
typedef struct {
    size_t count;
    odk_spine_world_state_context *items;
} odk_spine_world_state_context_list;

/* ------------------------------------------------------------------ */
/* Generated record API. Memory model:                                */
/*   *_init      zero the record (empty lists, NULL strings).         */
/*   *_from_json strict parse: every schema field required, unknown    */
/*               fields rejected; allocates; on error the record is    */
/*               reset.                                                */
/*   *_to_json   compact JSON, keys in schema field order; returns a   */
/*               malloc'd string (caller frees) or NULL.               */
/*   *_release   free all owned memory, then zero the record.          */
/* ------------------------------------------------------------------ */

void odk_spine_registration_ref_init(odk_spine_registration_ref *r);
void odk_spine_registration_ref_release(odk_spine_registration_ref *r);
char *odk_spine_registration_ref_to_json(const odk_spine_registration_ref *r);
odk_spine_status odk_spine_registration_ref_from_json(const char *text,
                                        odk_spine_registration_ref *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_registration_ref_from_dom(const odk_json_value *v,
                                       odk_spine_registration_ref *out,
                                       odk_spine_error *err);

void odk_spine_observation_init(odk_spine_observation *r);
void odk_spine_observation_release(odk_spine_observation *r);
char *odk_spine_observation_to_json(const odk_spine_observation *r);
odk_spine_status odk_spine_observation_from_json(const char *text,
                                        odk_spine_observation *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_observation_from_dom(const odk_json_value *v,
                                       odk_spine_observation *out,
                                       odk_spine_error *err);

void odk_spine_scheme_rule_init(odk_spine_scheme_rule *r);
void odk_spine_scheme_rule_release(odk_spine_scheme_rule *r);
char *odk_spine_scheme_rule_to_json(const odk_spine_scheme_rule *r);
odk_spine_status odk_spine_scheme_rule_from_json(const char *text,
                                        odk_spine_scheme_rule *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_scheme_rule_from_dom(const odk_json_value *v,
                                       odk_spine_scheme_rule *out,
                                       odk_spine_error *err);

void odk_spine_scheme_init(odk_spine_scheme *r);
void odk_spine_scheme_release(odk_spine_scheme *r);
char *odk_spine_scheme_to_json(const odk_spine_scheme *r);
odk_spine_status odk_spine_scheme_from_json(const char *text,
                                        odk_spine_scheme *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_scheme_from_dom(const odk_json_value *v,
                                       odk_spine_scheme *out,
                                       odk_spine_error *err);

void odk_spine_synonym_group_init(odk_spine_synonym_group *r);
void odk_spine_synonym_group_release(odk_spine_synonym_group *r);
char *odk_spine_synonym_group_to_json(const odk_spine_synonym_group *r);
odk_spine_status odk_spine_synonym_group_from_json(const char *text,
                                        odk_spine_synonym_group *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_synonym_group_from_dom(const odk_json_value *v,
                                       odk_spine_synonym_group *out,
                                       odk_spine_error *err);

void odk_spine_vocabulary_init(odk_spine_vocabulary *r);
void odk_spine_vocabulary_release(odk_spine_vocabulary *r);
char *odk_spine_vocabulary_to_json(const odk_spine_vocabulary *r);
odk_spine_status odk_spine_vocabulary_from_json(const char *text,
                                        odk_spine_vocabulary *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_vocabulary_from_dom(const odk_json_value *v,
                                       odk_spine_vocabulary *out,
                                       odk_spine_error *err);

void odk_spine_classification_init(odk_spine_classification *r);
void odk_spine_classification_release(odk_spine_classification *r);
char *odk_spine_classification_to_json(const odk_spine_classification *r);
odk_spine_status odk_spine_classification_from_json(const char *text,
                                        odk_spine_classification *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_classification_from_dom(const odk_json_value *v,
                                       odk_spine_classification *out,
                                       odk_spine_error *err);

void odk_spine_classification_result_init(odk_spine_classification_result *r);
void odk_spine_classification_result_release(odk_spine_classification_result *r);
char *odk_spine_classification_result_to_json(const odk_spine_classification_result *r);
odk_spine_status odk_spine_classification_result_from_json(const char *text,
                                        odk_spine_classification_result *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_classification_result_from_dom(const odk_json_value *v,
                                       odk_spine_classification_result *out,
                                       odk_spine_error *err);

void odk_spine_classification_interval_init(odk_spine_classification_interval *r);
void odk_spine_classification_interval_release(odk_spine_classification_interval *r);
char *odk_spine_classification_interval_to_json(const odk_spine_classification_interval *r);
odk_spine_status odk_spine_classification_interval_from_json(const char *text,
                                        odk_spine_classification_interval *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_classification_interval_from_dom(const odk_json_value *v,
                                       odk_spine_classification_interval *out,
                                       odk_spine_error *err);

void odk_spine_template_parameter_init(odk_spine_template_parameter *r);
void odk_spine_template_parameter_release(odk_spine_template_parameter *r);
char *odk_spine_template_parameter_to_json(const odk_spine_template_parameter *r);
odk_spine_status odk_spine_template_parameter_from_json(const char *text,
                                        odk_spine_template_parameter *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_template_parameter_from_dom(const odk_json_value *v,
                                       odk_spine_template_parameter *out,
                                       odk_spine_error *err);

void odk_spine_component_spec_init(odk_spine_component_spec *r);
void odk_spine_component_spec_release(odk_spine_component_spec *r);
char *odk_spine_component_spec_to_json(const odk_spine_component_spec *r);
odk_spine_status odk_spine_component_spec_from_json(const char *text,
                                        odk_spine_component_spec *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_component_spec_from_dom(const odk_json_value *v,
                                       odk_spine_component_spec *out,
                                       odk_spine_error *err);

void odk_spine_extraction_rule_init(odk_spine_extraction_rule *r);
void odk_spine_extraction_rule_release(odk_spine_extraction_rule *r);
char *odk_spine_extraction_rule_to_json(const odk_spine_extraction_rule *r);
odk_spine_status odk_spine_extraction_rule_from_json(const char *text,
                                        odk_spine_extraction_rule *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_extraction_rule_from_dom(const odk_json_value *v,
                                       odk_spine_extraction_rule *out,
                                       odk_spine_error *err);

void odk_spine_decomposition_template_init(odk_spine_decomposition_template *r);
void odk_spine_decomposition_template_release(odk_spine_decomposition_template *r);
char *odk_spine_decomposition_template_to_json(const odk_spine_decomposition_template *r);
odk_spine_status odk_spine_decomposition_template_from_json(const char *text,
                                        odk_spine_decomposition_template *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_decomposition_template_from_dom(const odk_json_value *v,
                                       odk_spine_decomposition_template *out,
                                       odk_spine_error *err);

void odk_spine_component_init(odk_spine_component *r);
void odk_spine_component_release(odk_spine_component *r);
char *odk_spine_component_to_json(const odk_spine_component *r);
odk_spine_status odk_spine_component_from_json(const char *text,
                                        odk_spine_component *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_component_from_dom(const odk_json_value *v,
                                       odk_spine_component *out,
                                       odk_spine_error *err);

void odk_spine_components_init(odk_spine_components *r);
void odk_spine_components_release(odk_spine_components *r);
char *odk_spine_components_to_json(const odk_spine_components *r);
odk_spine_status odk_spine_components_from_json(const char *text,
                                        odk_spine_components *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_components_from_dom(const odk_json_value *v,
                                       odk_spine_components *out,
                                       odk_spine_error *err);

void odk_spine_decision_pin_init(odk_spine_decision_pin *r);
void odk_spine_decision_pin_release(odk_spine_decision_pin *r);
char *odk_spine_decision_pin_to_json(const odk_spine_decision_pin *r);
odk_spine_status odk_spine_decision_pin_from_json(const char *text,
                                        odk_spine_decision_pin *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_decision_pin_from_dom(const odk_json_value *v,
                                       odk_spine_decision_pin *out,
                                       odk_spine_error *err);

void odk_spine_calibration_bin_init(odk_spine_calibration_bin *r);
void odk_spine_calibration_bin_release(odk_spine_calibration_bin *r);
char *odk_spine_calibration_bin_to_json(const odk_spine_calibration_bin *r);
odk_spine_status odk_spine_calibration_bin_from_json(const char *text,
                                        odk_spine_calibration_bin *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_calibration_bin_from_dom(const odk_json_value *v,
                                       odk_spine_calibration_bin *out,
                                       odk_spine_error *err);

void odk_spine_calibration_report_init(odk_spine_calibration_report *r);
void odk_spine_calibration_report_release(odk_spine_calibration_report *r);
char *odk_spine_calibration_report_to_json(const odk_spine_calibration_report *r);
odk_spine_status odk_spine_calibration_report_from_json(const char *text,
                                        odk_spine_calibration_report *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_calibration_report_from_dom(const odk_json_value *v,
                                       odk_spine_calibration_report *out,
                                       odk_spine_error *err);

void odk_spine_world_variable_init(odk_spine_world_variable *r);
void odk_spine_world_variable_release(odk_spine_world_variable *r);
char *odk_spine_world_variable_to_json(const odk_spine_world_variable *r);
odk_spine_status odk_spine_world_variable_from_json(const char *text,
                                        odk_spine_world_variable *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_world_variable_from_dom(const odk_json_value *v,
                                       odk_spine_world_variable *out,
                                       odk_spine_error *err);

void odk_spine_world_state_context_init(odk_spine_world_state_context *r);
void odk_spine_world_state_context_release(odk_spine_world_state_context *r);
char *odk_spine_world_state_context_to_json(const odk_spine_world_state_context *r);
odk_spine_status odk_spine_world_state_context_from_json(const char *text,
                                        odk_spine_world_state_context *out,
                                        odk_spine_error *err);
odk_spine_status odk_spine_world_state_context_from_dom(const odk_json_value *v,
                                       odk_spine_world_state_context *out,
                                       odk_spine_error *err);

#ifdef __cplusplus
}
#endif

#endif /* ODK_SPINE_TYPES_H */
