#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# This runner ships inside the aether repo at tests/; the .aether fixtures sit
# beside it and the examples are one directory up. AETHER_BIN defaults to the
# standalone build (./build/aether); callers can override it, e.g. the umbrella
# points it at build/bin/aether over this same corpus.
TESTS_DIR="$SCRIPT_DIR"
EX_DIR="$(cd "$SCRIPT_DIR/../examples" && pwd)"
AETHER_BIN="${AETHER_BIN:-$SCRIPT_DIR/../build/aether}"
# Run from the showcase example dir so its cwd-relative "agent_payload.json"
# resolves; every other fixture uses an absolute $TESTS_DIR path.
cd "$EX_DIR/showcase"
SMOKE_FIXTURE="$TESTS_DIR/smoke.aether"
NEG_ARRAY_FIXTURE="$TESTS_DIR/negative_array_literal.aether"
STRING_PARSE_PASS_FIXTURE="$TESTS_DIR/string_parse_pass.aether"
COMPOUND_LINES_PASS_FIXTURE="$TESTS_DIR/compound_lines_pass.aether"
TOON_ALIAS_PASS_FIXTURE="$TESTS_DIR/toon_alias_pass.aether"
ALIAS_STRING_LITERAL_PASS_FIXTURE="$TESTS_DIR/alias_string_literal_pass.aether"
CONTRACT_PASS_FIXTURE="$TESTS_DIR/contracts_pass.aether"
CONTRACT_LAYOUT_PASS_FIXTURE="$TESTS_DIR/contract_layout_pass.aether"
CONTRACT_STRING_LEN_PASS_FIXTURE="$TESTS_DIR/contract_string_len_pass.aether"
CONTRACT_COMMENT_PASS_FIXTURE="$TESTS_DIR/contract_annotation_comment_pass.aether"
COST_PASS_FIXTURE="$TESTS_DIR/cost_annotation_pass.aether"
COST_ZERO_FAIL_FIXTURE="$TESTS_DIR/cost_annotation_zero_fail.aether"
COST_UNIT_FAIL_FIXTURE="$TESTS_DIR/cost_annotation_unit_fail.aether"
COST_DETACHED_FAIL_FIXTURE="$TESTS_DIR/cost_annotation_detached_fail.aether"
COST_DUPLICATE_FAIL_FIXTURE="$TESTS_DIR/cost_annotation_duplicate_fail.aether"
CONTRACT_PRE_EMPTY_FAIL_FIXTURE="$TESTS_DIR/contract_annotation_pre_empty_fail.aether"
CONTRACT_POST_DETACHED_FAIL_FIXTURE="$TESTS_DIR/contract_annotation_post_detached_fail.aether"
CONTRACT_PURE_TRAILING_FAIL_FIXTURE="$TESTS_DIR/contract_annotation_pure_trailing_fail.aether"
CONTRACT_FAIL_PRE_FIXTURE="$TESTS_DIR/contracts_fail_pre.aether"
CONTRACT_FAIL_POST_FIXTURE="$TESTS_DIR/contracts_fail_post.aether"
CONTRACT_COLLECTION_RESULT_FAIL_FIXTURE="$TESTS_DIR/contract_collection_result_fail.aether"
CONTRACT_COLLECTION_LENGTH_PASS_FIXTURE="$TESTS_DIR/contract_collection_length_pass.aether"
EFFECTS_FAIL_FIXTURE="$TESTS_DIR/effects_fail_outside_fx.aether"
FX_CROSS_LINE_CALL_FAIL_FIXTURE="$TESTS_DIR/fx_cross_line_call_fail.aether"
FX_BRACE_NEXT_LINE_PASS_FIXTURE="$TESTS_DIR/fx_brace_next_line_pass.aether"
PRINT_ALIAS_PASS_FIXTURE="$TESTS_DIR/print_alias_pass.aether"
PRINT_ALIAS_FAIL_FIXTURE="$TESTS_DIR/print_alias_fail_outside_fx.aether"
TASK_HELPERS_PASS_FIXTURE="$TESTS_DIR/task_helpers_pass.aether"
TASK_ALIAS_FAIL_FIXTURE="$TESTS_DIR/task_alias_fail_outside_fx.aether"
SLEEP_ALIAS_PASS_FIXTURE="$TESTS_DIR/sleep_alias_pass.aether"
SLEEP_ALIAS_FAIL_FIXTURE="$TESTS_DIR/sleep_alias_fail_outside_fx.aether"
HAS_BUILTIN_ALIAS_PASS_FIXTURE="$TESTS_DIR/has_builtin_alias_pass.aether"
BUILTIN_QUERY_PASS_FIXTURE="$TESTS_DIR/builtin_query_pass.aether"
AI_HELPERS_PASS_FIXTURE="$TESTS_DIR/ai_helpers_pass.aether"
AI_ALIAS_FAIL_FIXTURE="$TESTS_DIR/ai_alias_fail_outside_fx.aether"
RUNTIME_LINE_MAPPING_FAIL_FIXTURE="$TESTS_DIR/runtime_line_mapping_fail.aether"
INFERRED_BINDINGS_PASS_FIXTURE="$TESTS_DIR/inferred_bindings_pass.aether"
INFERRED_CONST_PASS_FIXTURE="$TESTS_DIR/inferred_const_pass.aether"
INFERRED_LET_UNKNOWN_FAIL_FIXTURE="$TESTS_DIR/inferred_let_unknown_fail.aether"
SCOPED_BINDINGS_PASS_FIXTURE="$TESTS_DIR/scoped_bindings_pass.aether"
SCOPED_BINDINGS_FAIL_FIXTURE="$TESTS_DIR/scoped_bindings_fail.aether"
DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE="$TESTS_DIR/diagnostic_line_mapping_fail.aether"
FUNCTION_INFERENCE_SUPPORT_FIXTURE="$TESTS_DIR/function_inference_support"
FUNCTION_RETURN_INFERENCE_PASS_FIXTURE="$TESTS_DIR/function_return_inference_pass.aether"
FUNCTION_MISSING_RETURN_TYPE_FAIL_FIXTURE="$TESTS_DIR/function_missing_return_type_fail.aether"
FUNCTION_FORWARD_DECL_PASS_FIXTURE="$TESTS_DIR/function_forward_decl_pass.aether"
FUNCTION_MISSING_VALUE_RETURN_FAIL_FIXTURE="$TESTS_DIR/function_missing_value_return_fail.aether"
FUNCTION_EMPTY_RETURN_FAIL_FIXTURE="$TESTS_DIR/function_empty_return_fail.aether"
BACKEND_UNKNOWN_FIELD_CODED_FAIL_FIXTURE="$TESTS_DIR/backend_unknown_field_coded_fail.aether"
OBJECT_INFERENCE_PASS_FIXTURE="$TESTS_DIR/object_inference_pass.aether"
OBJECT_DEFAULT_INIT_PASS_FIXTURE="$TESTS_DIR/object_default_init_pass.aether"
STRING_LEN_INFERENCE_PASS_FIXTURE="$TESTS_DIR/string_len_inference_pass.aether"
LEN_PROPERTY_PASS_FIXTURE="$TESTS_DIR/len_property_pass.aether"
NUMERIC_EXPR_INFERENCE_PASS_FIXTURE="$TESTS_DIR/numeric_expr_inference_pass.aether"
REAL_ASSIGNMENT_FROM_MIXED_NUMERIC_EXPR_PASS_FIXTURE="$TESTS_DIR/real_assignment_from_mixed_numeric_expr_pass.aether"
INLINE_OBJECT_METHOD_INFERENCE_PASS_FIXTURE="$TESTS_DIR/inline_object_method_inference_pass.aether"
INLINE_OBJECT_METHOD_INFERENCE_COMMENT_PASS_FIXTURE="$TESTS_DIR/inline_object_method_inference_comment_pass.aether"
TUPLE_DESTRUCTURE_PASS_FIXTURE="$TESTS_DIR/tuple_destructure_pass.aether"
TUPLE_DESTRUCTURE_FORWARD_PASS_FIXTURE="$TESTS_DIR/tuple_destructure_forward_pass.aether"
TUPLE_POST_PASS_FIXTURE="$TESTS_DIR/tuple_post_pass.aether"
ARRAY_APPEND_PASS_FIXTURE="$TESTS_DIR/dynamic_array_append_pass.aether"
ARRAY_FIELD_INDEX_PASS_FIXTURE="$TESTS_DIR/array_field_index_pass.aether"
EXTENSION_CALL_ALIAS_PASS_FIXTURE="$TESTS_DIR/extension_call_alias_pass.aether"
EXTENSION_METHOD_DOT_CALL_PASS_FIXTURE="$TESTS_DIR/extension_method_dot_call_pass.aether"
TUPLE_DIRECT_BIND_FAIL_FIXTURE="$TESTS_DIR/tuple_return_unsupported_fail.aether"
TUPLE_BAD_DESTRUCTURE_FAIL_FIXTURE="$TESTS_DIR/tuple_let_destructure_unsupported_fail.aether"
TUPLE_POST_INVALID_RESULT_FAIL_FIXTURE="$TESTS_DIR/tuple_post_invalid_result_fail.aether"
PURE_PASS_FIXTURE="$TESTS_DIR/pure_pass.aether"
PURE_FAIL_EFFECTFUL_FIXTURE="$TESTS_DIR/pure_fail_effectful.aether"
PURE_FAIL_NON_PURE_CALL_FIXTURE="$TESTS_DIR/pure_fail_non_pure_call.aether"
PURE_CONTAINS_FX_FAIL_FIXTURE="$TESTS_DIR/pure_contains_fx_fail.aether"
IMPORT_MISSING_FAIL_FIXTURE="$TESTS_DIR/import_missing_fail.aether"
PAR_PASS_FIXTURE="$TESTS_DIR/par_pass.aether"
PAR_FAIL_NON_CALL_FIXTURE="$TESTS_DIR/par_fail_non_call.aether"
PAR_SHARED_RECORD_FAIL_FIXTURE="$TESTS_DIR/par_shared_record_fail.aether"
PAR_SHARED_TUPLE_CALL_PASS_FIXTURE="$TESTS_DIR/par_shared_tuple_call_pass.aether"
SOCKET_ECHO_PASS_FIXTURE="$TESTS_DIR/socket_echo_pass.aether"
METHOD_UNDEFINED_FAIL_FIXTURE="$TESTS_DIR/method_undefined_fail.aether"
UNKNOWN_CONSTRUCT_FAIL_FIXTURE="$TESTS_DIR/unknown_construct_fail.aether"
UNCLOSED_BLOCK_FAIL_FIXTURE="$TESTS_DIR/unclosed_block_fail.aether"
UNCLOSED_CALL_FAIL_FIXTURE="$TESTS_DIR/unclosed_call_fail.aether"
FIXED_SIZE_ARRAY_FAIL_FIXTURE="$TESTS_DIR/fixed_size_array_fail.aether"
WRITE_FORMAT_COLON_FAIL_FIXTURE="$TESTS_DIR/write_format_colon_fail.aether"
TUPLE_RECURSION_PASS_FIXTURE="$TESTS_DIR/tuple_recursion_pass.aether"
TUPLE_INDIRECT_RECURSION_PASS_FIXTURE="$TESTS_DIR/tuple_indirect_recursion_pass.aether"
RECURSION_PASS_FIXTURE="$TESTS_DIR/recursion_pass.aether"
RET_RECURSION_PASS_FIXTURE="$TESTS_DIR/ret_recursion_pass.aether"
RET_INT_DIVISION_PASS_FIXTURE="$TESTS_DIR/ret_int_division_pass.aether"
FOR_RANGE_PASS_FIXTURE="$TESTS_DIR/for_range_pass.aether"
LOOP_FORMS_PASS_FIXTURE="$TESTS_DIR/loop_forms_pass.aether"
ARRAY_RETURN_PASS_FIXTURE="$TESTS_DIR/array_return_pass.aether"
INLINE_IF_EXPR_PASS_FIXTURE="$TESTS_DIR/inline_if_expr_pass.aether"
INLINE_IF_CALL_ARGS_PASS_FIXTURE="$TESTS_DIR/inline_if_call_args_pass.aether"
MULTILINE_INLINE_IF_DECL_PASS_FIXTURE="$TESTS_DIR/multiline_inline_if_decl_pass.aether"
RETURN_OBJECT_INIT_PASS_FIXTURE="$TESTS_DIR/return_object_init_pass.aether"
STRING_EQ_ALIAS_PASS_FIXTURE="$TESTS_DIR/string_eq_alias_pass.aether"
MODULE_IMPORT_PASS_FIXTURE="$TESTS_DIR/module_import_pass.aether"
MODULE_PURE_EXPORT_DIRECT_PASS_FIXTURE="$TESTS_DIR/module_pure_export_direct_pass.aether"
MODULE_SUPPORT_FIXTURE="$TESTS_DIR/module_math"
MODULE_CONST_SUPPORT_FIXTURE="$TESTS_DIR/module_consts"
MODULE_CONST_IMPORT_PASS_FIXTURE="$TESTS_DIR/module_const_import_pass.aether"
MODULE_SHAPES_SUPPORT_FIXTURE="$TESTS_DIR/module_shapes"
MODULE_TYPE_FIELD_ACCESS_PASS_FIXTURE="$TESTS_DIR/module_type_field_access_pass.aether"
MODULE_TYPE_GLOBAL_LET_PASS_FIXTURE="$TESTS_DIR/module_type_global_let_pass.aether"
MODULE_CALC_SUPPORT_FIXTURE="$TESTS_DIR/module_calc"
MODULE_INTRAMODULE_CALL_PASS_FIXTURE="$TESTS_DIR/module_intramodule_call_pass.aether"
MODULE_SELF_QUALIFIED_CALL_PASS_FIXTURE="$TESTS_DIR/module_self_qualified_call_pass.aether"
MODULE_VOID_PARAM_SUPPORT_FIXTURE="$TESTS_DIR/module_void_param"
MODULE_TYPE_PARAM_VOID_BARE_CALL_PASS_FIXTURE="$TESTS_DIR/module_type_param_void_bare_call_pass.aether"
MODULE_HELPER_COLLISION_A_SUPPORT_FIXTURE="$TESTS_DIR/module_helper_collision_a"
MODULE_HELPER_COLLISION_B_SUPPORT_FIXTURE="$TESTS_DIR/module_helper_collision_b"
MODULE_PRIVATE_HELPER_COLLISION_PASS_FIXTURE="$TESTS_DIR/module_private_helper_collision_pass.aether"
MODULE_LIB_WITH_MAIN_SUPPORT_FIXTURE="$TESTS_DIR/module_lib_with_main"
MODULE_IMPORTED_MAIN_NOT_ENTRY_POINT_PASS_FIXTURE="$TESTS_DIR/module_imported_main_not_entry_point_pass.aether"
IF_LEADING_PAREN_SUBEXPR_PASS_FIXTURE="$TESTS_DIR/if_leading_paren_subexpr_pass.aether"
TOON_BLOCK_PASS_FIXTURE="$TESTS_DIR/toon_block_pass.aether"
TYPE_BLOCK_PASS_FIXTURE="$TESTS_DIR/type_block_pass.aether"
TYPE_FIELD_COMMA_FAIL_FIXTURE="$TESTS_DIR/type_field_comma_fail.aether"
RESERVED_FIELD_NAME_FAIL_FIXTURE="$TESTS_DIR/reserved_field_name_fail.aether"
RESERVED_METHOD_NAME_FAIL_FIXTURE="$TESTS_DIR/reserved_method_name_fail.aether"
RESERVED_NEW_METHOD_FAIL_FIXTURE="$TESTS_DIR/reserved_new_method_fail.aether"
TYPE_INIT_PASS_FIXTURE="$TESTS_DIR/type_init_pass.aether"
TYPE_INIT_PAREN_PASS_FIXTURE="$TESTS_DIR/type_init_paren_pass.aether"
TYPE_FIELD_DEFAULT_PASS_FIXTURE="$TESTS_DIR/type_field_default_pass.aether"
TYPE_FIELD_DEFAULT_NONCONST_FAIL_FIXTURE="$TESTS_DIR/type_field_default_nonconst_fail.aether"
TYPE_FIELD_DEFAULT_TYPE_MISMATCH_FAIL_FIXTURE="$TESTS_DIR/type_field_default_type_mismatch_fail.aether"
TYPE_METHOD_CONTRACTS_PASS_FIXTURE="$TESTS_DIR/type_method_contracts_pass.aether"
SELF_ALIAS_PASS_FIXTURE="$TESTS_DIR/self_alias_pass.aether"
SELF_MUTATION_PASS_FIXTURE="$TESTS_DIR/self_mutation_pass.aether"
METHOD_FIELD_INFERENCE_PASS_FIXTURE="$TESTS_DIR/method_field_inference_pass.aether"
INFERRED_OBJECT_MUTATION_PASS_FIXTURE="$TESTS_DIR/inferred_object_mutation_pass.aether"
ARRAY_RECORD_LITERAL_PASS_FIXTURE="$TESTS_DIR/array_record_literal_pass.aether"
SELF_CONDITION_METHOD_PASS_FIXTURE="$TESTS_DIR/self_condition_method_pass.aether"
LOOP_RANGE_BOOL_BOUND_FAIL_FIXTURE="$TESTS_DIR/loop_range_bool_bound_fail.aether"
TEXT_FIELD_METHOD_PARAM_PASS_FIXTURE="$TESTS_DIR/text_field_method_param_pass.aether"
TOON_JSON_HELPERS_PASS_FIXTURE="$TESTS_DIR/toon_json_helpers_pass.aether"
TOON_HANDLE_HELPERS_PASS_FIXTURE="$TESTS_DIR/toon_handle_helpers_pass.aether"
TOON_HANDLE_NIL_COMPARE_PASS_FIXTURE="$TESTS_DIR/toon_handle_nil_compare_pass.aether"
TOON_VARIABLE_PARSE_PASS_FIXTURE="$TESTS_DIR/toon_variable_parse_pass.aether"
HAS_TOON_ALIAS_PASS_FIXTURE="$TESTS_DIR/has_toon_alias_pass.aether"
TOON_HANDLE_ARITH_FAIL_FIXTURE="$TESTS_DIR/toon_handle_arithmetic_fail.aether"
TOON_HANDLE_CROSS_ASSIGN_FAIL_FIXTURE="$TESTS_DIR/toon_handle_cross_assign_fail.aether"
TOON_HANDLE_KIND_DOC_AS_NODE_FAIL_FIXTURE="$TESTS_DIR/toon_handle_kind_fail_doc_as_node.aether"
TOON_HANDLE_KIND_NODE_AS_DOC_FAIL_FIXTURE="$TESTS_DIR/toon_handle_kind_fail_node_as_doc.aether"
TOON_HANDLE_DECL_FAIL_DOC_TYPE_FIXTURE="$TESTS_DIR/toon_handle_decl_fail_doc_type.aether"
TOON_HANDLE_DECL_FAIL_NODE_TYPE_FIXTURE="$TESTS_DIR/toon_handle_decl_fail_node_type.aether"
TOON_HANDLE_REASSIGN_FAIL_FIXTURE="$TESTS_DIR/toon_handle_reassign_fail.aether"
TOON_SCALAR_DECL_FAIL_TEXT_TYPE_FIXTURE="$TESTS_DIR/toon_scalar_decl_fail_text_type.aether"
TOON_SCALAR_REASSIGN_FAIL_FIXTURE="$TESTS_DIR/toon_scalar_reassign_fail.aether"
TOON_SCALAR_CROSS_ASSIGN_FAIL_FIXTURE="$TESTS_DIR/toon_scalar_cross_assign_fail.aether"
TOON_KEY_ARG_TYPE_FAIL_FIXTURE="$TESTS_DIR/toon_key_arg_type_fail.aether"
TOON_OBJECT_KEY_ARG_TYPE_FAIL_FIXTURE="$TESTS_DIR/toon_object_key_arg_type_fail.aether"
TOON_INDEX_ARG_TYPE_FAIL_FIXTURE="$TESTS_DIR/toon_index_arg_type_fail.aether"
TOON_PARSE_ARG_TYPE_FAIL_FIXTURE="$TESTS_DIR/toon_parse_arg_type_fail.aether"
TOON_PARSE_FILE_ARG_TYPE_FAIL_FIXTURE="$TESTS_DIR/toon_parse_file_arg_type_fail.aether"
TOON_SHAPE_SCALAR_DECL_FAIL_FIXTURE="$TESTS_DIR/toon_shape_scalar_decl_fail.aether"
TOON_REAL_DECL_FAIL_FIXTURE="$TESTS_DIR/toon_real_decl_fail.aether"
TOON_TYPE_DECL_FAIL_FIXTURE="$TESTS_DIR/toon_type_decl_fail.aether"
TOON_PRESENCE_DECL_FAIL_FIXTURE="$TESTS_DIR/toon_presence_decl_fail.aether"
TOON_DEFAULTS_DECL_FAIL_FIXTURE="$TESTS_DIR/toon_defaults_decl_fail.aether"
TOON_COMMENT_ARITH_PASS_FIXTURE="$TESTS_DIR/toon_comment_arithmetic_pass.aether"
TOON_NESTED_HELPERS_PASS_FIXTURE="$TESTS_DIR/toon_nested_helpers_pass.aether"
TOON_SINGLE_CHAR_KEY_PASS_FIXTURE="$TESTS_DIR/toon_single_char_key_pass.aether"
TOON_OBJECT_ROOT_ITER_FAIL_FIXTURE="$TESTS_DIR/toon_object_root_iteration_fail.aether"
MSTREAM_HANDLE_PASS_FIXTURE="$TESTS_DIR/mstream_handle_pass.aether"
MSTREAM_DECL_INT_FAIL_FIXTURE="$TESTS_DIR/mstream_decl_int_fail.aether"
MSTREAM_HANDLE_ARITH_FAIL_FIXTURE="$TESTS_DIR/mstream_handle_arithmetic_fail.aether"
MSTREAM_CROSS_KIND_FAIL_FIXTURE="$TESTS_DIR/mstream_cross_kind_fail.aether"
SWAP_SHADOW_BUILTIN_PASS_FIXTURE="$TESTS_DIR/swap_shadow_builtin_pass.aether"
SWAP_BUILTIN_UNSHADOWED_FAIL_FIXTURE="$TESTS_DIR/swap_builtin_unshadowed_fail.aether"
HTTP_SESSION_FX_FAIL_FIXTURE="$TESTS_DIR/http_session_fx_fail.aether"
HTTP_MSTREAM_COMPILE_PASS_FIXTURE="$TESTS_DIR/http_mstream_compile_pass.aether"
LEGACY_METHOD_CALL_SHADOW_PASS_FIXTURE="$TESTS_DIR/legacy_method_call_shadow_pass.aether"
LEGACY_METHOD_CALL_UNDEFINED_FAIL_FIXTURE="$TESTS_DIR/legacy_method_call_undefined_fail.aether"
SHOWCASE_EXAMPLE="$EX_DIR/showcase/agent_report"

if [ ! -x "$AETHER_BIN" ]; then
    echo "missing aether binary: $AETHER_BIN" >&2
    exit 1
fi

# A few fixtures exercise the OpenAI ext-builtin, which depends on curl and is
# absent from minimal standalone builds (pscal-core ships SDL/curl/sqlite OFF by
# default). Detect it once via the inventory dump and skip those fixtures when it
# is not compiled in, the same way the showcase tolerates a missing yyjson. The
# umbrella build links OpenAI in, so it still runs the full set over this corpus.
HAS_OPENAI=0
if "$AETHER_BIN" --dump-ext-builtins 2>/dev/null | grep -qi 'OpenAIChatCompletions'; then
    HAS_OPENAI=1
else
    echo "[skip] OpenAI ext-builtin not present in this binary; AI fixtures skipped" >&2
fi

for fixture in \
    "$SMOKE_FIXTURE" \
    "$COMPOUND_LINES_PASS_FIXTURE" \
    "$CONTRACT_PASS_FIXTURE" \
    "$CONTRACT_LAYOUT_PASS_FIXTURE" \
    "$CONTRACT_STRING_LEN_PASS_FIXTURE" \
    "$CONTRACT_COMMENT_PASS_FIXTURE" \
    "$COST_PASS_FIXTURE" \
    "$COST_ZERO_FAIL_FIXTURE" \
    "$COST_UNIT_FAIL_FIXTURE" \
    "$COST_DETACHED_FAIL_FIXTURE" \
    "$COST_DUPLICATE_FAIL_FIXTURE" \
    "$CONTRACT_PRE_EMPTY_FAIL_FIXTURE" \
    "$CONTRACT_POST_DETACHED_FAIL_FIXTURE" \
    "$CONTRACT_PURE_TRAILING_FAIL_FIXTURE" \
    "$CONTRACT_FAIL_PRE_FIXTURE" \
    "$CONTRACT_FAIL_POST_FIXTURE" \
    "$CONTRACT_COLLECTION_RESULT_FAIL_FIXTURE" \
    "$CONTRACT_COLLECTION_LENGTH_PASS_FIXTURE" \
    "$EFFECTS_FAIL_FIXTURE" \
    "$PRINT_ALIAS_PASS_FIXTURE" \
    "$PRINT_ALIAS_FAIL_FIXTURE" \
    "$TASK_HELPERS_PASS_FIXTURE" \
    "$TASK_ALIAS_FAIL_FIXTURE" \
    "$HAS_BUILTIN_ALIAS_PASS_FIXTURE" \
    "$AI_HELPERS_PASS_FIXTURE" \
    "$AI_ALIAS_FAIL_FIXTURE" \
    "$RUNTIME_LINE_MAPPING_FAIL_FIXTURE" \
    "$INFERRED_BINDINGS_PASS_FIXTURE" \
    "$INFERRED_CONST_PASS_FIXTURE" \
    "$INFERRED_LET_UNKNOWN_FAIL_FIXTURE" \
    "$SCOPED_BINDINGS_PASS_FIXTURE" \
    "$SCOPED_BINDINGS_FAIL_FIXTURE" \
    "$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE" \
    "$FUNCTION_INFERENCE_SUPPORT_FIXTURE" \
    "$FUNCTION_RETURN_INFERENCE_PASS_FIXTURE" \
    "$FUNCTION_MISSING_RETURN_TYPE_FAIL_FIXTURE" \
    "$FUNCTION_FORWARD_DECL_PASS_FIXTURE" \
    "$FUNCTION_MISSING_VALUE_RETURN_FAIL_FIXTURE" \
    "$FUNCTION_EMPTY_RETURN_FAIL_FIXTURE" \
    "$BACKEND_UNKNOWN_FIELD_CODED_FAIL_FIXTURE" \
    "$OBJECT_INFERENCE_PASS_FIXTURE" \
    "$OBJECT_DEFAULT_INIT_PASS_FIXTURE" \
    "$STRING_LEN_INFERENCE_PASS_FIXTURE" \
    "$LEN_PROPERTY_PASS_FIXTURE" \
    "$NUMERIC_EXPR_INFERENCE_PASS_FIXTURE" \
    "$REAL_ASSIGNMENT_FROM_MIXED_NUMERIC_EXPR_PASS_FIXTURE" \
    "$INLINE_OBJECT_METHOD_INFERENCE_PASS_FIXTURE" \
    "$INLINE_OBJECT_METHOD_INFERENCE_COMMENT_PASS_FIXTURE" \
    "$TUPLE_DESTRUCTURE_PASS_FIXTURE" \
    "$TUPLE_DESTRUCTURE_FORWARD_PASS_FIXTURE" \
    "$TUPLE_POST_PASS_FIXTURE" \
    "$ARRAY_APPEND_PASS_FIXTURE" \
    "$EXTENSION_CALL_ALIAS_PASS_FIXTURE" \
    "$EXTENSION_METHOD_DOT_CALL_PASS_FIXTURE" \
    "$TUPLE_DIRECT_BIND_FAIL_FIXTURE" \
    "$TUPLE_BAD_DESTRUCTURE_FAIL_FIXTURE" \
    "$PURE_PASS_FIXTURE" \
    "$PURE_FAIL_EFFECTFUL_FIXTURE" \
    "$PURE_FAIL_NON_PURE_CALL_FIXTURE" \
    "$IMPORT_MISSING_FAIL_FIXTURE" \
    "$PAR_PASS_FIXTURE" \
    "$PAR_FAIL_NON_CALL_FIXTURE" \
    "$PAR_SHARED_RECORD_FAIL_FIXTURE" \
    "$PAR_SHARED_TUPLE_CALL_PASS_FIXTURE" \
    "$METHOD_UNDEFINED_FAIL_FIXTURE" \
    "$UNKNOWN_CONSTRUCT_FAIL_FIXTURE" \
    "$UNCLOSED_BLOCK_FAIL_FIXTURE" \
    "$UNCLOSED_CALL_FAIL_FIXTURE" \
    "$FIXED_SIZE_ARRAY_FAIL_FIXTURE" \
    "$WRITE_FORMAT_COLON_FAIL_FIXTURE" \
    "$TUPLE_RECURSION_PASS_FIXTURE" \
    "$TUPLE_INDIRECT_RECURSION_PASS_FIXTURE" \
    "$RECURSION_PASS_FIXTURE" \
    "$RET_RECURSION_PASS_FIXTURE" \
    "$FOR_RANGE_PASS_FIXTURE" \
    "$LOOP_FORMS_PASS_FIXTURE" \
    "$ARRAY_RETURN_PASS_FIXTURE" \
    "$INLINE_IF_EXPR_PASS_FIXTURE" \
    "$INLINE_IF_CALL_ARGS_PASS_FIXTURE" \
    "$MULTILINE_INLINE_IF_DECL_PASS_FIXTURE" \
    "$RETURN_OBJECT_INIT_PASS_FIXTURE" \
    "$STRING_EQ_ALIAS_PASS_FIXTURE" \
    "$MODULE_IMPORT_PASS_FIXTURE" \
    "$MODULE_SUPPORT_FIXTURE" \
    "$MODULE_CONST_SUPPORT_FIXTURE" \
    "$MODULE_CONST_IMPORT_PASS_FIXTURE" \
    "$MODULE_SHAPES_SUPPORT_FIXTURE" \
    "$MODULE_TYPE_FIELD_ACCESS_PASS_FIXTURE" \
    "$MODULE_TYPE_GLOBAL_LET_PASS_FIXTURE" \
    "$MODULE_CALC_SUPPORT_FIXTURE" \
    "$MODULE_INTRAMODULE_CALL_PASS_FIXTURE" \
    "$MODULE_SELF_QUALIFIED_CALL_PASS_FIXTURE" \
    "$MODULE_VOID_PARAM_SUPPORT_FIXTURE" \
    "$MODULE_TYPE_PARAM_VOID_BARE_CALL_PASS_FIXTURE" \
    "$MODULE_HELPER_COLLISION_A_SUPPORT_FIXTURE" \
    "$MODULE_HELPER_COLLISION_B_SUPPORT_FIXTURE" \
    "$MODULE_PRIVATE_HELPER_COLLISION_PASS_FIXTURE" \
    "$MODULE_LIB_WITH_MAIN_SUPPORT_FIXTURE" \
    "$MODULE_IMPORTED_MAIN_NOT_ENTRY_POINT_PASS_FIXTURE" \
    "$TOON_BLOCK_PASS_FIXTURE" \
    "$TYPE_BLOCK_PASS_FIXTURE" \
    "$TYPE_FIELD_COMMA_FAIL_FIXTURE" \
    "$RESERVED_FIELD_NAME_FAIL_FIXTURE" \
    "$RESERVED_METHOD_NAME_FAIL_FIXTURE" \
    "$RESERVED_NEW_METHOD_FAIL_FIXTURE" \
    "$TYPE_INIT_PASS_FIXTURE" \
    "$TYPE_INIT_PAREN_PASS_FIXTURE" \
    "$TYPE_FIELD_DEFAULT_PASS_FIXTURE" \
    "$TYPE_FIELD_DEFAULT_NONCONST_FAIL_FIXTURE" \
    "$TYPE_FIELD_DEFAULT_TYPE_MISMATCH_FAIL_FIXTURE" \
    "$TYPE_METHOD_CONTRACTS_PASS_FIXTURE" \
    "$SELF_ALIAS_PASS_FIXTURE" \
    "$SELF_MUTATION_PASS_FIXTURE" \
    "$METHOD_FIELD_INFERENCE_PASS_FIXTURE" \
    "$INFERRED_OBJECT_MUTATION_PASS_FIXTURE" \
    "$SELF_CONDITION_METHOD_PASS_FIXTURE" \
    "$TEXT_FIELD_METHOD_PARAM_PASS_FIXTURE" \
    "$TOON_JSON_HELPERS_PASS_FIXTURE" \
    "$TOON_HANDLE_HELPERS_PASS_FIXTURE" \
    "$TOON_HANDLE_NIL_COMPARE_PASS_FIXTURE" \
    "$TOON_VARIABLE_PARSE_PASS_FIXTURE" \
    "$HAS_TOON_ALIAS_PASS_FIXTURE" \
    "$TOON_HANDLE_ARITH_FAIL_FIXTURE" \
    "$TOON_HANDLE_CROSS_ASSIGN_FAIL_FIXTURE" \
    "$TOON_HANDLE_KIND_DOC_AS_NODE_FAIL_FIXTURE" \
    "$TOON_HANDLE_KIND_NODE_AS_DOC_FAIL_FIXTURE" \
    "$TOON_HANDLE_DECL_FAIL_DOC_TYPE_FIXTURE" \
    "$TOON_HANDLE_DECL_FAIL_NODE_TYPE_FIXTURE" \
    "$TOON_HANDLE_REASSIGN_FAIL_FIXTURE" \
    "$TOON_SCALAR_DECL_FAIL_TEXT_TYPE_FIXTURE" \
    "$TOON_SCALAR_REASSIGN_FAIL_FIXTURE" \
    "$TOON_SCALAR_CROSS_ASSIGN_FAIL_FIXTURE" \
    "$TOON_KEY_ARG_TYPE_FAIL_FIXTURE" \
    "$TOON_OBJECT_KEY_ARG_TYPE_FAIL_FIXTURE" \
    "$TOON_INDEX_ARG_TYPE_FAIL_FIXTURE" \
    "$TOON_PARSE_ARG_TYPE_FAIL_FIXTURE" \
    "$TOON_PARSE_FILE_ARG_TYPE_FAIL_FIXTURE" \
    "$TOON_SHAPE_SCALAR_DECL_FAIL_FIXTURE" \
    "$TOON_REAL_DECL_FAIL_FIXTURE" \
    "$TOON_TYPE_DECL_FAIL_FIXTURE" \
    "$TOON_PRESENCE_DECL_FAIL_FIXTURE" \
    "$TOON_DEFAULTS_DECL_FAIL_FIXTURE" \
    "$TOON_COMMENT_ARITH_PASS_FIXTURE" \
    "$TOON_NESTED_HELPERS_PASS_FIXTURE" \
    "$TOON_SINGLE_CHAR_KEY_PASS_FIXTURE" \
    "$TOON_OBJECT_ROOT_ITER_FAIL_FIXTURE" \
    "$MSTREAM_HANDLE_PASS_FIXTURE" \
    "$MSTREAM_DECL_INT_FAIL_FIXTURE" \
    "$MSTREAM_HANDLE_ARITH_FAIL_FIXTURE" \
    "$MSTREAM_CROSS_KIND_FAIL_FIXTURE" \
    "$SWAP_SHADOW_BUILTIN_PASS_FIXTURE" \
    "$SWAP_BUILTIN_UNSHADOWED_FAIL_FIXTURE" \
    "$HTTP_SESSION_FX_FAIL_FIXTURE" \
    "$HTTP_MSTREAM_COMPILE_PASS_FIXTURE" \
    "$LEGACY_METHOD_CALL_SHADOW_PASS_FIXTURE" \
    "$LEGACY_METHOD_CALL_UNDEFINED_FAIL_FIXTURE" \
    "$LOOP_RANGE_BOOL_BOUND_FAIL_FIXTURE" \
    "$SHOWCASE_EXAMPLE"
do
    if [ ! -f "$fixture" ]; then
        echo "missing fixture: $fixture" >&2
        exit 1
    fi
done

"$AETHER_BIN" --no-cache --no-run "$SMOKE_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache --dump-ast-json "$SMOKE_FIXTURE" >/dev/null
# Compound lines: a type field + method, and a fn body, sharing one physical line
# must parse (aetherNormalizeCompoundLines splits them). Regression for the two
# SYN-001 mis-parses (dropped method / untranslated inline body).
"$AETHER_BIN" --no-cache "$COMPOUND_LINES_PASS_FIXTURE" >/tmp/aether_compound_lines_pass.out
if ! grep -qx "7 positive" /tmp/aether_compound_lines_pass.out; then
    echo "unexpected compound-line output (regression: run-on type members / inline fn bodies)" >&2
    cat /tmp/aether_compound_lines_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$CONTRACT_PASS_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache "$CONTRACT_LAYOUT_PASS_FIXTURE" >/tmp/aether_contract_layout_pass.out
if ! grep -qx "42" /tmp/aether_contract_layout_pass.out; then
    echo "unexpected contract layout output" >&2
    cat /tmp/aether_contract_layout_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$CONTRACT_STRING_LEN_PASS_FIXTURE" >/tmp/aether_contract_string_len_pass.out
if ! grep -qx "10" /tmp/aether_contract_string_len_pass.out; then
    echo "unexpected contract string_len output" >&2
    cat /tmp/aether_contract_string_len_pass.out >&2
    exit 1
fi
# A `//` line comment trailing a @pre/@post annotation must be stripped before
# the contract expression is lowered to a guard (comment text is not code), and
# a `//` inside a string literal in the expression must be preserved. A leak in
# either direction turns this into a compile error instead of the 1/42 output.
"$AETHER_BIN" --no-cache "$CONTRACT_COMMENT_PASS_FIXTURE" >/tmp/aether_contract_comment_pass.out
printf '1\n42\n' >/tmp/aether_contract_comment_expected.out
if ! cmp -s /tmp/aether_contract_comment_expected.out /tmp/aether_contract_comment_pass.out; then
    echo "unexpected contract annotation comment output (regression: // comment leaked into guard, or in-literal // stripped)" >&2
    cat /tmp/aether_contract_comment_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$COST_PASS_FIXTURE" >/tmp/aether_cost_pass.out
if ! grep -qx "42" /tmp/aether_cost_pass.out; then
    echo "unexpected cost annotation output" >&2
    cat /tmp/aether_cost_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$PRINT_ALIAS_PASS_FIXTURE" >/tmp/aether_print_alias_pass.out
printf 'Aether print aliases\n' >/tmp/aether_print_alias_expected.out
if ! cmp -s /tmp/aether_print_alias_expected.out /tmp/aether_print_alias_pass.out; then
    echo "unexpected print alias output" >&2
    cat /tmp/aether_print_alias_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache --no-run "$ARRAY_RETURN_PASS_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache "$INLINE_IF_EXPR_PASS_FIXTURE" >/tmp/aether_inline_if_expr_pass.out
if ! grep -qx "42" /tmp/aether_inline_if_expr_pass.out; then
    echo "unexpected inline if expression output" >&2
    cat /tmp/aether_inline_if_expr_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$NEG_ARRAY_FIXTURE" >/tmp/aether_neg_array_pass.out
if ! grep -qx "neg_array = -5,-2,-4" /tmp/aether_neg_array_pass.out; then
    echo "unexpected negative array literal output (regression: negative Int[] elements)" >&2
    cat /tmp/aether_neg_array_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$STRING_PARSE_PASS_FIXTURE" >/tmp/aether_string_parse_pass.out
printf '19\ntrue\n99\n' >/tmp/aether_string_parse_expected.out
if ! cmp -s /tmp/aether_string_parse_expected.out /tmp/aether_string_parse_pass.out; then
    echo "unexpected string/parse stdlib output (split/parse_int/parse_bool/itoa)" >&2
    cat /tmp/aether_string_parse_pass.out >&2
    exit 1
fi
# TOON aliases must resolve and dispatch to the canonical toon_* API. Accept the
# no-yyjson fallback too, since the point is that the alias names compile + run.
"$AETHER_BIN" --no-cache "$TOON_ALIAS_PASS_FIXTURE" >/tmp/aether_toon_alias_pass.out
if ! grep -qxE 'Aether 42|toon unavailable' /tmp/aether_toon_alias_pass.out; then
    echo "unexpected TOON alias output (parse_json/root_node/lookup_string/lookup_int/close_doc)" >&2
    cat /tmp/aether_toon_alias_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$ALIAS_STRING_LITERAL_PASS_FIXTURE" >/tmp/aether_alias_string_literal_pass.out
printf 'call sleep(5) now\nstring_eq(a,b) or len(x) or has_toon() inline\nlen=3 same=true\n' >/tmp/aether_alias_string_literal_expected.out
if ! cmp -s /tmp/aether_alias_string_literal_expected.out /tmp/aether_alias_string_literal_pass.out; then
    echo "alias lowering corrupted a string literal (or an alias outside a string broke)" >&2
    cat /tmp/aether_alias_string_literal_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$INLINE_IF_CALL_ARGS_PASS_FIXTURE" >/tmp/aether_inline_if_call_args_pass.out
printf 'status:ready\n' >/tmp/aether_inline_if_call_args_expected.out
if ! cmp -s /tmp/aether_inline_if_call_args_expected.out /tmp/aether_inline_if_call_args_pass.out; then
    echo "unexpected inline if call-arg output" >&2
    cat /tmp/aether_inline_if_call_args_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$MULTILINE_INLINE_IF_DECL_PASS_FIXTURE" >/tmp/aether_multiline_inline_if_decl_pass.out
if ! grep -qx "ready" /tmp/aether_multiline_inline_if_decl_pass.out; then
    echo "unexpected multiline inline if decl output" >&2
    cat /tmp/aether_multiline_inline_if_decl_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$RETURN_OBJECT_INIT_PASS_FIXTURE" >/tmp/aether_return_object_init_pass.out
printf '7\nready\n' >/tmp/aether_return_object_init_expected.out
if ! cmp -s /tmp/aether_return_object_init_expected.out /tmp/aether_return_object_init_pass.out; then
    echo "unexpected return object init output" >&2
    cat /tmp/aether_return_object_init_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$STRING_EQ_ALIAS_PASS_FIXTURE" >/tmp/aether_string_eq_alias_pass.out
if ! grep -qx "ok" /tmp/aether_string_eq_alias_pass.out; then
    echo "unexpected string_eq alias output" >&2
    cat /tmp/aether_string_eq_alias_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TASK_HELPERS_PASS_FIXTURE" >/tmp/aether_task_helpers_pass.out
if ! grep -q '^named_ok = true$' /tmp/aether_task_helpers_pass.out; then
    echo "unexpected task helper named output" >&2
    cat /tmp/aether_task_helpers_pass.out >&2
    exit 1
fi
if ! grep -q '^pooled_ok = true$' /tmp/aether_task_helpers_pass.out; then
    echo "unexpected task helper pooled output" >&2
    cat /tmp/aether_task_helpers_pass.out >&2
    exit 1
fi
if ! grep -q '^lookup_match = true$' /tmp/aether_task_helpers_pass.out; then
    echo "unexpected task helper lookup output" >&2
    cat /tmp/aether_task_helpers_pass.out >&2
    exit 1
fi
if ! grep -Eq '^stats = [0-9]+$' /tmp/aether_task_helpers_pass.out; then
    echo "unexpected task helper stats output" >&2
    cat /tmp/aether_task_helpers_pass.out >&2
    exit 1
fi
if ! grep -Eq '^has_ai = (true|false)$' /tmp/aether_task_helpers_pass.out; then
    echo "unexpected task helper has_ai output" >&2
    cat /tmp/aether_task_helpers_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$SLEEP_ALIAS_PASS_FIXTURE" >/tmp/aether_sleep_alias_pass.out
printf 'before\nafter\n' >/tmp/aether_sleep_alias_expected.out
if ! cmp -s /tmp/aether_sleep_alias_expected.out /tmp/aether_sleep_alias_pass.out; then
    echo "unexpected sleep alias output" >&2
    cat /tmp/aether_sleep_alias_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$HAS_BUILTIN_ALIAS_PASS_FIXTURE" >/tmp/aether_has_builtin_alias_pass.out
if ! grep -Eq '^(true|false)$' /tmp/aether_has_builtin_alias_pass.out; then
    echo "unexpected has_builtin alias output" >&2
    cat /tmp/aether_has_builtin_alias_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$BUILTIN_QUERY_PASS_FIXTURE" >/tmp/aether_builtin_query_pass.out
printf 'true\ntrue\ntrue\ntrue\n' >/tmp/aether_builtin_query_expected.out
if ! cmp -s /tmp/aether_builtin_query_expected.out /tmp/aether_builtin_query_pass.out; then
    echo "unexpected builtin query output" >&2
    cat /tmp/aether_builtin_query_pass.out >&2
    exit 1
fi
if [ "$HAS_OPENAI" = 1 ]; then
"$AETHER_BIN" --no-cache "$AI_HELPERS_PASS_FIXTURE" >/tmp/aether_ai_helpers_pass.out
if ! grep -Eq '^has_ai = (true|false)$' /tmp/aether_ai_helpers_pass.out; then
    echo "unexpected ai helper capability output" >&2
    cat /tmp/aether_ai_helpers_pass.out >&2
    exit 1
fi
if ! grep -Eq '^has_openai = (true|false)$' /tmp/aether_ai_helpers_pass.out; then
    echo "unexpected ai helper builtin output" >&2
    cat /tmp/aether_ai_helpers_pass.out >&2
    exit 1
fi
else
    echo "[skip] ai_helpers_pass: OpenAI ext-builtin not present" >&2
fi
"$AETHER_BIN" --no-cache "$INFERRED_BINDINGS_PASS_FIXTURE" >/tmp/aether_inferred_bindings_pass.out
if grep -qx "yyjson unavailable" /tmp/aether_inferred_bindings_pass.out; then
    :
else
    printf 'Aether\n42\n3.500000\ntrue\n2\n' >/tmp/aether_inferred_bindings_expected.out
    if ! cmp -s /tmp/aether_inferred_bindings_expected.out /tmp/aether_inferred_bindings_pass.out; then
        echo "unexpected inferred binding output" >&2
        cat /tmp/aether_inferred_bindings_pass.out >&2
        exit 1
    fi
fi
"$AETHER_BIN" --no-cache "$INFERRED_CONST_PASS_FIXTURE" >/tmp/aether_inferred_const_pass.out
if ! grep -qx "Aether" /tmp/aether_inferred_const_pass.out; then
    echo "unexpected inferred const output" >&2
    cat /tmp/aether_inferred_const_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$FUNCTION_RETURN_INFERENCE_PASS_FIXTURE" >/tmp/aether_function_return_inference_pass.out
printf 'Aether\n42\n' >/tmp/aether_function_return_inference_expected.out
if ! cmp -s /tmp/aether_function_return_inference_expected.out /tmp/aether_function_return_inference_pass.out; then
    echo "unexpected function return inference output" >&2
    cat /tmp/aether_function_return_inference_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$FUNCTION_FORWARD_DECL_PASS_FIXTURE" >/tmp/aether_function_forward_decl_pass.out
if ! grep -qx "42" /tmp/aether_function_forward_decl_pass.out; then
    echo "unexpected function forward declaration output" >&2
    cat /tmp/aether_function_forward_decl_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$OBJECT_INFERENCE_PASS_FIXTURE" >/tmp/aether_object_inference_pass.out
printf '42\ntrue\n' >/tmp/aether_object_inference_expected.out
if ! cmp -s /tmp/aether_object_inference_expected.out /tmp/aether_object_inference_pass.out; then
    echo "unexpected object inference output" >&2
    cat /tmp/aether_object_inference_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$OBJECT_DEFAULT_INIT_PASS_FIXTURE" >/tmp/aether_object_default_init_pass.out
printf '1\n2\n' >/tmp/aether_object_default_init_expected.out
if ! cmp -s /tmp/aether_object_default_init_expected.out /tmp/aether_object_default_init_pass.out; then
    echo "unexpected object default init output" >&2
    cat /tmp/aether_object_default_init_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$STRING_LEN_INFERENCE_PASS_FIXTURE" >/tmp/aether_string_len_inference_pass.out
printf 'true\nfalse\n' >/tmp/aether_string_len_inference_expected.out
if ! cmp -s /tmp/aether_string_len_inference_expected.out /tmp/aether_string_len_inference_pass.out; then
    echo "unexpected string_len inference output" >&2
    cat /tmp/aether_string_len_inference_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$LEN_PROPERTY_PASS_FIXTURE" >/tmp/aether_len_property_pass.out
printf '6\n7\n2\n' >/tmp/aether_len_property_expected.out
if ! cmp -s /tmp/aether_len_property_expected.out /tmp/aether_len_property_pass.out; then
    echo "unexpected len property output" >&2
    cat /tmp/aether_len_property_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$NUMERIC_EXPR_INFERENCE_PASS_FIXTURE" >/tmp/aether_numeric_expr_inference_pass.out
printf '8.000000\n' >/tmp/aether_numeric_expr_inference_expected.out
if ! cmp -s /tmp/aether_numeric_expr_inference_expected.out /tmp/aether_numeric_expr_inference_pass.out; then
    echo "unexpected numeric expression inference output" >&2
    cat /tmp/aether_numeric_expr_inference_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$INLINE_OBJECT_METHOD_INFERENCE_PASS_FIXTURE" >/tmp/aether_inline_object_method_inference_pass.out
printf 'true\n' >/tmp/aether_inline_object_method_inference_expected.out
if ! cmp -s /tmp/aether_inline_object_method_inference_expected.out /tmp/aether_inline_object_method_inference_pass.out; then
    echo "unexpected inline object method inference output" >&2
    cat /tmp/aether_inline_object_method_inference_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$INLINE_OBJECT_METHOD_INFERENCE_COMMENT_PASS_FIXTURE" >/tmp/aether_inline_object_method_inference_comment_pass.out
if ! cmp -s /tmp/aether_inline_object_method_inference_expected.out /tmp/aether_inline_object_method_inference_comment_pass.out; then
    echo "unexpected inline object method inference with comment output" >&2
    cat /tmp/aether_inline_object_method_inference_comment_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TUPLE_DESTRUCTURE_PASS_FIXTURE" >/tmp/aether_tuple_destructure_pass.out
printf '1\n2\n' >/tmp/aether_tuple_destructure_expected.out
if ! cmp -s /tmp/aether_tuple_destructure_expected.out /tmp/aether_tuple_destructure_pass.out; then
    echo "unexpected tuple destructure output" >&2
    cat /tmp/aether_tuple_destructure_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TUPLE_DESTRUCTURE_FORWARD_PASS_FIXTURE" >/tmp/aether_tuple_destructure_forward_pass.out
printf 'answer 42\n' >/tmp/aether_tuple_destructure_forward_expected.out
if ! cmp -s /tmp/aether_tuple_destructure_forward_expected.out /tmp/aether_tuple_destructure_forward_pass.out; then
    echo "unexpected forward tuple destructure output" >&2
    cat /tmp/aether_tuple_destructure_forward_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TUPLE_POST_PASS_FIXTURE" >/tmp/aether_tuple_post_pass.out
printf 'lo = 3\nhi = 8\n' >/tmp/aether_tuple_post_expected.out
if ! cmp -s /tmp/aether_tuple_post_expected.out /tmp/aether_tuple_post_pass.out; then
    echo "unexpected tuple post output" >&2
    cat /tmp/aether_tuple_post_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$ARRAY_APPEND_PASS_FIXTURE" >/tmp/aether_array_append_pass.out
printf '2\n7\n9\n' >/tmp/aether_array_append_expected.out
if ! cmp -s /tmp/aether_array_append_expected.out /tmp/aether_array_append_pass.out; then
    echo "unexpected dynamic array append output" >&2
    cat /tmp/aether_array_append_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$ARRAY_FIELD_INDEX_PASS_FIXTURE" >/tmp/aether_array_field_index_pass.out
printf '14\n' >/tmp/aether_array_field_index_expected.out
if ! cmp -s /tmp/aether_array_field_index_expected.out /tmp/aether_array_field_index_pass.out; then
    echo "unexpected array field index output" >&2
    cat /tmp/aether_array_field_index_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$EXTENSION_CALL_ALIAS_PASS_FIXTURE" >/tmp/aether_extension_call_alias_pass.out
printf '5.000000\n4\n' >/tmp/aether_extension_call_alias_expected.out
if ! cmp -s /tmp/aether_extension_call_alias_expected.out /tmp/aether_extension_call_alias_pass.out; then
    echo "unexpected extension call alias output" >&2
    cat /tmp/aether_extension_call_alias_pass.out >&2
    exit 1
fi
# A dotted call to an extension method (recv.m()) is valid UFCS and must NOT be
# a false [SCOPE-001] from the undefined-method check (the extension decl stays
# un-mangled, so the check also accepts a plain `fn m(self: T)`).
"$AETHER_BIN" --no-cache "$EXTENSION_METHOD_DOT_CALL_PASS_FIXTURE" >/tmp/aether_extension_method_dot_call_pass.out 2>&1
if ! grep -qx "12" /tmp/aether_extension_method_dot_call_pass.out; then
    echo "unexpected extension-method dot-call output (false SCOPE-001 regression?)" >&2
    cat /tmp/aether_extension_method_dot_call_pass.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$TUPLE_DIRECT_BIND_FAIL_FIXTURE" >/tmp/aether_tuple_direct_bind_fail.out 2>&1; then
    echo "expected tuple direct bind failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "tuple-return calls must be destructured directly" /tmp/aether_tuple_direct_bind_fail.out; then
    echo "missing tuple direct bind failure message" >&2
    cat /tmp/aether_tuple_direct_bind_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$TUPLE_BAD_DESTRUCTURE_FAIL_FIXTURE" >/tmp/aether_tuple_bad_destructure_fail.out 2>&1; then
    echo "expected tuple bad destructure failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "tuple destructuring target is not a known tuple-return function" /tmp/aether_tuple_bad_destructure_fail.out; then
    echo "missing tuple bad destructure failure message" >&2
    cat /tmp/aether_tuple_bad_destructure_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$TUPLE_POST_INVALID_RESULT_FAIL_FIXTURE" >/tmp/aether_tuple_post_invalid_result_fail.out 2>&1; then
    echo "expected tuple @post positional failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "tuple-return @post checks must reference slots explicitly" /tmp/aether_tuple_post_invalid_result_fail.out; then
    echo "missing tuple @post positional failure message" >&2
    cat /tmp/aether_tuple_post_invalid_result_fail.out >&2
    exit 1
fi
# The tuple destructuring diagnostic must carry the real TUP-001 code (not the
# former placeholder `feature`) so --diagnostics-json feeds the code->guide map,
# and must include an actionable hint pointing at the record alternative.
if "$AETHER_BIN" --diagnostics-json --no-cache "$TUPLE_BAD_DESTRUCTURE_FAIL_FIXTURE" >/tmp/aether_tuple_bad_destructure_json.out 2>&1; then
    echo "expected tuple destructure diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"code":"TUP-001"' /tmp/aether_tuple_bad_destructure_json.out; then
    echo "missing tuple diagnostics-json code TUP-001 (regression: placeholder code?)" >&2
    cat /tmp/aether_tuple_bad_destructure_json.out >&2
    exit 1
fi
if ! grep -q '"hint":"the callee must be a top-level tuple-return function' /tmp/aether_tuple_bad_destructure_json.out; then
    echo "missing tuple diagnostics-json hint (record alternative)" >&2
    cat /tmp/aether_tuple_bad_destructure_json.out >&2
    exit 1
fi
# A @post contract comparing a whole collection to a scalar (`result > 0` on a
# `T[]` return) must be rejected at COMPILE time (ANN-001), not lowered to a
# guard that crashes the VM with "Operands not comparable" at runtime.
if "$AETHER_BIN" --no-cache "$CONTRACT_COLLECTION_RESULT_FAIL_FIXTURE" >/tmp/aether_contract_collection_result_fail.out 2>&1; then
    echo "expected collection-vs-scalar contract failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "arrays and scalars are not comparable" /tmp/aether_contract_collection_result_fail.out; then
    echo "missing collection-vs-scalar contract failure message" >&2
    cat /tmp/aether_contract_collection_result_fail.out >&2
    exit 1
fi
if ! grep -q "ANN-001" /tmp/aether_contract_collection_result_fail.out; then
    echo "collection contract diagnostic missing ANN-001 code" >&2
    cat /tmp/aether_contract_collection_result_fail.out >&2
    exit 1
fi
if grep -q "Operands not comparable" /tmp/aether_contract_collection_result_fail.out; then
    echo "collection contract crashed at runtime instead of failing at compile time" >&2
    cat /tmp/aether_contract_collection_result_fail.out >&2
    exit 1
fi
# The documented fix -- `length(result) > 0` -- must still compile and run.
"$AETHER_BIN" --no-cache "$CONTRACT_COLLECTION_LENGTH_PASS_FIXTURE" >/tmp/aether_contract_collection_length_pass.out
if ! grep -qx "3" /tmp/aether_contract_collection_length_pass.out; then
    echo "unexpected collection length contract output" >&2
    cat /tmp/aether_contract_collection_length_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$PURE_PASS_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache --no-run "$PAR_PASS_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache "$FOR_RANGE_PASS_FIXTURE" >/tmp/aether_for_range_pass.out
if ! grep -qx "10" /tmp/aether_for_range_pass.out; then
    echo "unexpected for-range output" >&2
    cat /tmp/aether_for_range_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$LOOP_FORMS_PASS_FIXTURE" >/tmp/aether_loop_forms_pass.out
printf 'total = 9\nspins = 2\n' >/tmp/aether_loop_forms_expected.out
if ! cmp -s /tmp/aether_loop_forms_expected.out /tmp/aether_loop_forms_pass.out; then
    echo "unexpected loop forms output" >&2
    cat /tmp/aether_loop_forms_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$MODULE_IMPORT_PASS_FIXTURE" >/tmp/aether_module_import_pass.out
if ! grep -qx "42" /tmp/aether_module_import_pass.out; then
    echo "unexpected module import output" >&2
    cat /tmp/aether_module_import_pass.out >&2
    exit 1
fi
# R1 regression: @pure above `export fn` must compile when the module FILE is
# the direct compile target (not only when imported via use).
if ! "$AETHER_BIN" --no-cache "$MODULE_PURE_EXPORT_DIRECT_PASS_FIXTURE" >/tmp/aether_module_pure_export_direct.out 2>&1; then
    echo "direct compile of a module with @pure export fn failed" >&2
    cat /tmp/aether_module_pure_export_direct.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$MODULE_CONST_IMPORT_PASS_FIXTURE" >/tmp/aether_module_const_import_pass.out
printf 'Aether\n42\n' >/tmp/aether_module_const_import_expected.out
if ! cmp -s /tmp/aether_module_const_import_expected.out /tmp/aether_module_const_import_pass.out; then
    echo "unexpected module const import output" >&2
    cat /tmp/aether_module_const_import_pass.out >&2
    exit 1
fi
# Regression: a `type` block exported from a module must stay resolvable --
# for var-decl typing and for field access -- while the importing program is
# analyzed. See rea's rea_module_type_field_access_test for the underlying fix.
"$AETHER_BIN" --no-cache "$MODULE_TYPE_FIELD_ACCESS_PASS_FIXTURE" >/tmp/aether_module_type_field_access_pass.out
if ! grep -qx "7" /tmp/aether_module_type_field_access_pass.out; then
    echo "unexpected module type field access output" >&2
    cat /tmp/aether_module_type_field_access_pass.out >&2
    exit 1
fi
# Regression: a top-level (global-scope) `let` typed with a module-exported
# type must resolve like a function-local one. Before rea's fix the global
# path kept the parse-time TYPE_UNKNOWN ("Cannot assign POINTER to
# UNKNOWN_VAR_TYPE"). See rea's rea_module_type_global_var_decl_test.
"$AETHER_BIN" --no-cache "$MODULE_TYPE_GLOBAL_LET_PASS_FIXTURE" >/tmp/aether_module_type_global_let_pass.out
if ! grep -qx "7" /tmp/aether_module_type_global_let_pass.out; then
    echo "unexpected module type global let output" >&2
    cat /tmp/aether_module_type_global_let_pass.out >&2
    exit 1
fi
# Regression: two functions declared in the same `mod { }` block must be able
# to call each other by their bare (unqualified) names, just like two
# top-level functions can. This used to fail with a bogus "Function 'timesTwo'
# expects 0 arguments" compiler error: Aether's own parser registered every
# function under its bare name regardless of module context, so the call site
# found that stale, arity-0 stub instead of ever reaching the correctly
# module-qualified symbol rea's semantic analysis registers.
"$AETHER_BIN" --no-cache "$MODULE_INTRAMODULE_CALL_PASS_FIXTURE" >/tmp/aether_module_intramodule_call_pass.out
if ! grep -qx "12" /tmp/aether_module_intramodule_call_pass.out; then
    echo "unexpected intra-module call output" >&2
    cat /tmp/aether_module_intramodule_call_pass.out >&2
    exit 1
fi
# Regression: a module function calling a sibling export through the
# module's own qualified name (e.g. `Calc.timesTwo(x)` from inside `mod
# Calc`) must resolve just like an external qualified caller would. This
# used to fail with "identifier 'Calc' not in scope": the qualified-call
# receiver is resolved against modules reached through `use`/`#import`, and
# a module never imports itself. See rea's rea_module_self_qualified_call_test.
"$AETHER_BIN" --no-cache "$MODULE_SELF_QUALIFIED_CALL_PASS_FIXTURE" >/tmp/aether_module_self_qualified_call_pass.out
if ! grep -qx "12" /tmp/aether_module_self_qualified_call_pass.out; then
    echo "unexpected self-qualified module call output" >&2
    cat /tmp/aether_module_self_qualified_call_pass.out >&2
    exit 1
fi
# Regression: a Void-returning function taking a parameter typed with a
# record exported from a different module, called as a bare statement
# (return value discarded), used to fail with "expects type VOID but got
# POINTER". The parameter's type is registered into procedure_table as an
# unresolved AST_TYPE_REFERENCE snapshot at parse time, before the module's
# `use` clause loads it; a bare-statement call reads that stale snapshot
# straight from procedure_table instead of the (correctly healed) live
# tree. See rea's semantic.c resolveForwardClassRefsInProcedureTable and
# https://github.com/emkey1/rea/issues/6.
"$AETHER_BIN" --no-cache "$MODULE_TYPE_PARAM_VOID_BARE_CALL_PASS_FIXTURE" >/tmp/aether_module_type_param_void_bare_call_pass.out
if ! grep -qx "42" /tmp/aether_module_type_param_void_bare_call_pass.out; then
    echo "unexpected void-param bare-call module output" >&2
    cat /tmp/aether_module_type_param_void_bare_call_pass.out >&2
    exit 1
fi
# Regression for https://github.com/emkey1/rea/issues/5 (finding #8): two
# modules each declaring their own private (non-exported) helper() of the
# same bare name must not collide. registerFunctionSymbol() (ast_parser.c)
# used to add an unscoped bare-name alias for ANY dotted symbol name --
# including "ModuleName.funcname" qualified names, not just "Class.method"
# ones -- so the second module's private helper silently reused (and never
# updated) the first module's alias, making ModB.helper unreachable by its
# own bare name and ModA.helper's implementation run in its place.
"$AETHER_BIN" --no-cache "$MODULE_PRIVATE_HELPER_COLLISION_PASS_FIXTURE" >/tmp/aether_module_private_helper_collision_pass.out
if [ "$(cat /tmp/aether_module_private_helper_collision_pass.out)" != "$(printf '2\n101')" ]; then
    echo "unexpected private-helper collision output" >&2
    cat /tmp/aether_module_private_helper_collision_pass.out >&2
    exit 1
fi
# Regression for https://github.com/emkey1/rea/issues/5 (finding #8): a
# `use`d file's own top-level main() (declared outside any `mod { }` block,
# e.g. for that file's own standalone testing) must not shadow the
# importer's own main() as the program's entry point. The bare "main"
# symbol used to be reused-and-overwritten across independently-parsed
# files just like the private-helper case above.
"$AETHER_BIN" --no-cache "$MODULE_IMPORTED_MAIN_NOT_ENTRY_POINT_PASS_FIXTURE" >/tmp/aether_module_imported_main_not_entry_point_pass.out
if ! grep -qx "consumer: 8" /tmp/aether_module_imported_main_not_entry_point_pass.out; then
    echo "unexpected imported-main entry-point output" >&2
    cat /tmp/aether_module_imported_main_not_entry_point_pass.out >&2
    exit 1
fi
# Regression: `if (expr) && more { }` used to fail to parse. parseIfStmt
# special-cased a leading '(' as always wrapping the *whole* condition,
# so it consumed the '(' itself, called parseExpr (which only saw `a` and
# stopped at the matching ')'), then treated that ')' as closing the
# condition -- leaving `&& !c` dangling before the '{' and producing a
# bogus "unexpected token in block" error. Fixed by dropping the special
# case and handing the whole condition to parseExpr, exactly like parseLoop
# already does, since the general expression grammar parses balanced
# parens anywhere within it. See aether#7.
"$AETHER_BIN" --no-cache "$IF_LEADING_PAREN_SUBEXPR_PASS_FIXTURE" >/tmp/aether_if_leading_paren_subexpr_pass.out
if ! grep -qx "yes" /tmp/aether_if_leading_paren_subexpr_pass.out; then
    echo "unexpected if-leading-paren-subexpr output" >&2
    cat /tmp/aether_if_leading_paren_subexpr_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$SHOWCASE_EXAMPLE" >/tmp/aether_showcase_example.out
if grep -qx "yyjson unavailable" /tmp/aether_showcase_example.out; then
    :
else
    printf 'job 0: planner / ready / 95\njob 1: writer / review / 81\njob 2: tester / review / 72\njob 3: auditor / blocked / 55\ntotal = 4\nready = 1\nreview = 2\nblocked = 1\n' >/tmp/aether_showcase_example_expected.out
    if ! cmp -s /tmp/aether_showcase_example_expected.out /tmp/aether_showcase_example.out; then
        echo "unexpected Aether showcase output" >&2
        cat /tmp/aether_showcase_example.out >&2
        exit 1
    fi
fi
"$AETHER_BIN" --no-cache "$TOON_BLOCK_PASS_FIXTURE" >/tmp/aether_toon_block_pass.out
printf 'users[2]{id,name,role}:\n  1,Ada,admin\n  2,Bob,user\n' >/tmp/aether_toon_block_expected.out
if ! cmp -s /tmp/aether_toon_block_expected.out /tmp/aether_toon_block_pass.out; then
    echo "unexpected TOON block output" >&2
    cat /tmp/aether_toon_block_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TYPE_BLOCK_PASS_FIXTURE" >/tmp/aether_type_block_pass.out
if ! grep -qx "42" /tmp/aether_type_block_pass.out; then
    echo "unexpected type block output" >&2
    cat /tmp/aether_type_block_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TYPE_INIT_PASS_FIXTURE" >/tmp/aether_type_init_pass.out
if ! grep -qx "42" /tmp/aether_type_init_pass.out; then
    echo "unexpected type init output" >&2
    cat /tmp/aether_type_init_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TYPE_INIT_PAREN_PASS_FIXTURE" >/tmp/aether_type_init_paren_pass.out
if ! grep -qx "7" /tmp/aether_type_init_paren_pass.out; then
    echo "unexpected paren type init output" >&2
    cat /tmp/aether_type_init_paren_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TYPE_METHOD_CONTRACTS_PASS_FIXTURE" >/tmp/aether_type_method_contracts_pass.out
printf 'circle=78.539816\nrect=24.000000\n' >/tmp/aether_type_method_contracts_expected.out
if ! cmp -s /tmp/aether_type_method_contracts_expected.out /tmp/aether_type_method_contracts_pass.out; then
    echo "unexpected type method contract output" >&2
    cat /tmp/aether_type_method_contracts_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$SELF_ALIAS_PASS_FIXTURE" >/tmp/aether_self_alias_pass.out
printf '41\n42\n' >/tmp/aether_self_alias_expected.out
if ! cmp -s /tmp/aether_self_alias_expected.out /tmp/aether_self_alias_pass.out; then
    echo "unexpected self alias output" >&2
    cat /tmp/aether_self_alias_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$SELF_MUTATION_PASS_FIXTURE" >/tmp/aether_self_mutation_pass.out
if ! grep -qx "42" /tmp/aether_self_mutation_pass.out; then
    echo "unexpected self mutation output" >&2
    cat /tmp/aether_self_mutation_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$METHOD_FIELD_INFERENCE_PASS_FIXTURE" >/tmp/aether_method_field_inference_pass.out
if ! grep -qx "3" /tmp/aether_method_field_inference_pass.out; then
    echo "unexpected method field inference output" >&2
    cat /tmp/aether_method_field_inference_pass.out >&2
    exit 1
fi
# Inferred object binding + statement-level `-> Void` method call: `let c = new
# Counter();` (no annotation) must resolve the pointer-backed receiver so `c.inc();`
# type-checks, matching `let c: Counter = ...`. Regression for "argument 1 to
# 'c.inc' expects type POINTER but got VOID" (inferred-let record type left UNKNOWN).
"$AETHER_BIN" --no-cache "$INFERRED_OBJECT_MUTATION_PASS_FIXTURE" >/tmp/aether_inferred_object_mutation_pass.out
if ! grep -qx "42" /tmp/aether_inferred_object_mutation_pass.out; then
    echo "unexpected inferred object mutation output (regression: inferred receiver POINTER/VOID)" >&2
    cat /tmp/aether_inferred_object_mutation_pass.out >&2
    exit 1
fi
# Bare object literal `T { f: v }` used as a general expression (array
# element, call argument), not just directly after `let x: T =`. Regression
# for "[SYN-001] Expected ']' to close array literal" on `[T{...}, T{...}]`.
"$AETHER_BIN" --no-cache "$ARRAY_RECORD_LITERAL_PASS_FIXTURE" >/tmp/aether_array_record_literal_pass.out
printf '3\n4\n9\n25\n2\n100\n' >/tmp/aether_array_record_literal_expected.out
if ! cmp -s /tmp/aether_array_record_literal_expected.out /tmp/aether_array_record_literal_pass.out; then
    echo "unexpected array-record-literal output (regression: object literal as a non-let-position expression)" >&2
    cat /tmp/aether_array_record_literal_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$SELF_CONDITION_METHOD_PASS_FIXTURE" >/tmp/aether_self_condition_method_pass.out
printf '35\nIN_STOCK\n' >/tmp/aether_self_condition_method_expected.out
if ! cmp -s /tmp/aether_self_condition_method_expected.out /tmp/aether_self_condition_method_pass.out; then
    echo "unexpected self condition method output" >&2
    cat /tmp/aether_self_condition_method_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TEXT_FIELD_METHOD_PARAM_PASS_FIXTURE" >/tmp/aether_text_field_method_param_pass.out
printf 'true\nAlice\n150\n' >/tmp/aether_text_field_method_param_expected.out
if ! cmp -s /tmp/aether_text_field_method_param_expected.out /tmp/aether_text_field_method_param_pass.out; then
    echo "unexpected text field method parameter output" >&2
    cat /tmp/aether_text_field_method_param_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TOON_JSON_HELPERS_PASS_FIXTURE" >/tmp/aether_toon_json_helpers_pass.out
if grep -qx "yyjson unavailable" /tmp/aether_toon_json_helpers_pass.out; then
    :
else
    printf 'Rea\n3\n1\n' >/tmp/aether_toon_json_helpers_expected.out
    if ! cmp -s /tmp/aether_toon_json_helpers_expected.out /tmp/aether_toon_json_helpers_pass.out; then
        echo "unexpected TOON helper output" >&2
        cat /tmp/aether_toon_json_helpers_pass.out >&2
        exit 1
    fi
fi
"$AETHER_BIN" --no-cache "$TOON_HANDLE_HELPERS_PASS_FIXTURE" >/tmp/aether_toon_handle_helpers_pass.out
if grep -qx "yyjson unavailable" /tmp/aether_toon_handle_helpers_pass.out; then
    :
else
    printf '2\nBob\n2\n' >/tmp/aether_toon_handle_helpers_expected.out
    if ! cmp -s /tmp/aether_toon_handle_helpers_expected.out /tmp/aether_toon_handle_helpers_pass.out; then
        echo "unexpected TOON handle helper output" >&2
        cat /tmp/aether_toon_handle_helpers_pass.out >&2
        exit 1
    fi
fi
"$AETHER_BIN" --no-cache "$TOON_VARIABLE_PARSE_PASS_FIXTURE" >/tmp/aether_toon_variable_parse_pass.out
if grep -qx "yyjson unavailable" /tmp/aether_toon_variable_parse_pass.out; then
    :
else
    printf 'Aether\n42\n' >/tmp/aether_toon_variable_parse_expected.out
    if ! cmp -s /tmp/aether_toon_variable_parse_expected.out /tmp/aether_toon_variable_parse_pass.out; then
        echo "unexpected TOON variable parse output" >&2
        cat /tmp/aether_toon_variable_parse_pass.out >&2
        exit 1
    fi
fi
"$AETHER_BIN" --no-cache "$HAS_TOON_ALIAS_PASS_FIXTURE" >/tmp/aether_has_toon_alias_pass.out
if ! grep -Eq '^(toon-ready|toon-missing)$' /tmp/aether_has_toon_alias_pass.out; then
    echo "unexpected has_toon alias output" >&2
    cat /tmp/aether_has_toon_alias_pass.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_ARITH_FAIL_FIXTURE" >/tmp/aether_toon_handle_arith_fail.out 2>&1; then
    echo "expected TOON handle arithmetic failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "opaque TOON handle 'doc' cannot be used in arithmetic expressions" /tmp/aether_toon_handle_arith_fail.out; then
    echo "missing TOON handle arithmetic failure message" >&2
    cat /tmp/aether_toon_handle_arith_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_CROSS_ASSIGN_FAIL_FIXTURE" >/tmp/aether_toon_handle_cross_assign_fail.out 2>&1; then
    echo "expected TOON handle cross-assignment failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "cannot assign ToonDoc handle 'doc' to ToonNode binding" /tmp/aether_toon_handle_cross_assign_fail.out; then
    echo "missing TOON handle cross-assignment failure message" >&2
    cat /tmp/aether_toon_handle_cross_assign_fail.out >&2
    exit 1
fi
# The opaque cross-assign must carry [TOON-001] so --diagnostics-json feeds the
# code->guide repair loop (regression: this semantic family used to be uncoded).
if "$AETHER_BIN" --diagnostics-json --no-cache "$TOON_HANDLE_CROSS_ASSIGN_FAIL_FIXTURE" >/tmp/aether_toon_handle_cross_assign_json.out 2>&1; then
    echo "expected TOON handle cross-assignment diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"code":"TOON-001"' /tmp/aether_toon_handle_cross_assign_json.out; then
    echo "missing TOON-001 code on TOON handle cross-assignment (regression: uncoded semantic diagnostic?)" >&2
    cat /tmp/aether_toon_handle_cross_assign_json.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_KIND_DOC_AS_NODE_FAIL_FIXTURE" >/tmp/aether_toon_handle_kind_doc_as_node_fail.out 2>&1; then
    echo "expected TOON doc-as-node failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_text_value' expects a ToonNode handle, but 'doc' is ToonDoc" /tmp/aether_toon_handle_kind_doc_as_node_fail.out; then
    echo "missing TOON doc-as-node failure message" >&2
    cat /tmp/aether_toon_handle_kind_doc_as_node_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_KIND_NODE_AS_DOC_FAIL_FIXTURE" >/tmp/aether_toon_handle_kind_node_as_doc_fail.out 2>&1; then
    echo "expected TOON node-as-doc failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_close' expects a ToonDoc handle, but 'root' is ToonNode" /tmp/aether_toon_handle_kind_node_as_doc_fail.out; then
    echo "missing TOON node-as-doc failure message" >&2
    cat /tmp/aether_toon_handle_kind_node_as_doc_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_DECL_FAIL_DOC_TYPE_FIXTURE" >/tmp/aether_toon_handle_decl_fail_doc_type.out 2>&1; then
    echo "expected TOON doc declaration type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'doc' must use ToonDoc when initialized from 'toon_parse'" /tmp/aether_toon_handle_decl_fail_doc_type.out; then
    echo "missing TOON doc declaration type failure message" >&2
    cat /tmp/aether_toon_handle_decl_fail_doc_type.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_DECL_FAIL_NODE_TYPE_FIXTURE" >/tmp/aether_toon_handle_decl_fail_node_type.out 2>&1; then
    echo "expected TOON node declaration type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'root' must use ToonNode when initialized from 'toon_root'" /tmp/aether_toon_handle_decl_fail_node_type.out; then
    echo "missing TOON node declaration type failure message" >&2
    cat /tmp/aether_toon_handle_decl_fail_node_type.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_REASSIGN_FAIL_FIXTURE" >/tmp/aether_toon_handle_reassign_fail.out 2>&1; then
    echo "expected TOON handle reassignment failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'current' must use ToonDoc when initialized from 'toon_parse'" /tmp/aether_toon_handle_reassign_fail.out; then
    echo "missing TOON handle reassignment failure message" >&2
    cat /tmp/aether_toon_handle_reassign_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_SCALAR_DECL_FAIL_TEXT_TYPE_FIXTURE" >/tmp/aether_toon_scalar_decl_fail_text_type.out 2>&1; then
    echo "expected TOON scalar declaration type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'wrong' must use Text when initialized from 'toon_get_text'" /tmp/aether_toon_scalar_decl_fail_text_type.out; then
    echo "missing TOON scalar declaration type failure message" >&2
    cat /tmp/aether_toon_scalar_decl_fail_text_type.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_SCALAR_REASSIGN_FAIL_FIXTURE" >/tmp/aether_toon_scalar_reassign_fail.out 2>&1; then
    echo "expected TOON scalar reassignment type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'enabled' must use Text when initialized from 'toon_get_text'" /tmp/aether_toon_scalar_reassign_fail.out; then
    echo "missing TOON scalar reassignment type failure message" >&2
    cat /tmp/aether_toon_scalar_reassign_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_SCALAR_CROSS_ASSIGN_FAIL_FIXTURE" >/tmp/aether_toon_scalar_cross_assign_fail.out 2>&1; then
    echo "expected TOON scalar cross-assignment failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "cannot assign Text binding 'name' to Bool binding 'enabled'" /tmp/aether_toon_scalar_cross_assign_fail.out; then
    echo "missing TOON scalar cross-assignment failure message" >&2
    cat /tmp/aether_toon_scalar_cross_assign_fail.out >&2
    exit 1
fi
# The scalar assignment mismatch must carry [TYPE-001] so --diagnostics-json
# feeds the code->guide repair loop (regression: used to be uncoded).
if "$AETHER_BIN" --diagnostics-json --no-cache "$TOON_SCALAR_CROSS_ASSIGN_FAIL_FIXTURE" >/tmp/aether_toon_scalar_cross_assign_json.out 2>&1; then
    echo "expected TOON scalar cross-assignment diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"code":"TYPE-001"' /tmp/aether_toon_scalar_cross_assign_json.out; then
    echo "missing TYPE-001 code on scalar cross-assignment (regression: uncoded semantic diagnostic?)" >&2
    cat /tmp/aether_toon_scalar_cross_assign_json.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_KEY_ARG_TYPE_FAIL_FIXTURE" >/tmp/aether_toon_key_arg_type_fail.out 2>&1; then
    echo "expected TOON key argument type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_get_text' expects a Text second argument, but 'badKey' is Int" /tmp/aether_toon_key_arg_type_fail.out; then
    echo "missing TOON key argument type failure message" >&2
    cat /tmp/aether_toon_key_arg_type_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_OBJECT_KEY_ARG_TYPE_FAIL_FIXTURE" >/tmp/aether_toon_object_key_arg_type_fail.out 2>&1; then
    echo "expected TOON object-key argument type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_key' expects a Text second argument, but 'badKey' is Bool" /tmp/aether_toon_object_key_arg_type_fail.out; then
    echo "missing TOON object-key argument type failure message" >&2
    cat /tmp/aether_toon_object_key_arg_type_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_INDEX_ARG_TYPE_FAIL_FIXTURE" >/tmp/aether_toon_index_arg_type_fail.out 2>&1; then
    echo "expected TOON index argument type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_at' expects a Int second argument, but 'badIndex' is Text" /tmp/aether_toon_index_arg_type_fail.out; then
    echo "missing TOON index argument type failure message" >&2
    cat /tmp/aether_toon_index_arg_type_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_PARSE_ARG_TYPE_FAIL_FIXTURE" >/tmp/aether_toon_parse_arg_type_fail.out 2>&1; then
    echo "expected TOON parse argument type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_parse' expects a Text or TOON first argument, but 'badPayload' is Int" /tmp/aether_toon_parse_arg_type_fail.out; then
    echo "missing TOON parse argument type failure message" >&2
    cat /tmp/aether_toon_parse_arg_type_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_PARSE_FILE_ARG_TYPE_FAIL_FIXTURE" >/tmp/aether_toon_parse_file_arg_type_fail.out 2>&1; then
    echo "expected TOON parse_file argument type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_parse_file' expects a Text first argument, but 'badPath' is Bool" /tmp/aether_toon_parse_file_arg_type_fail.out; then
    echo "missing TOON parse_file argument type failure message" >&2
    cat /tmp/aether_toon_parse_file_arg_type_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_SHAPE_SCALAR_DECL_FAIL_FIXTURE" >/tmp/aether_toon_shape_scalar_decl_fail.out 2>&1; then
    echo "expected TOON shape scalar declaration type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'wrongLen' must use Int when initialized from 'toon_len'" /tmp/aether_toon_shape_scalar_decl_fail.out; then
    echo "missing TOON length declaration type failure message" >&2
    cat /tmp/aether_toon_shape_scalar_decl_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_REAL_DECL_FAIL_FIXTURE" >/tmp/aether_toon_real_decl_fail.out 2>&1; then
    echo "expected TOON real declaration type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'wrong' must use Real when initialized from 'toon_get_real'" /tmp/aether_toon_real_decl_fail.out; then
    echo "missing TOON real declaration type failure message" >&2
    cat /tmp/aether_toon_real_decl_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_TYPE_DECL_FAIL_FIXTURE" >/tmp/aether_toon_type_decl_fail.out 2>&1; then
    echo "expected TOON type inspection declaration failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'wrongType' must use Text when initialized from 'toon_type'" /tmp/aether_toon_type_decl_fail.out; then
    echo "missing TOON type() declaration type failure message" >&2
    cat /tmp/aether_toon_type_decl_fail.out >&2
    exit 1
fi
if ! grep -q "binding for 'wrongArr' must use Bool when initialized from 'toon_is_arr'" /tmp/aether_toon_type_decl_fail.out; then
    echo "missing TOON is_arr declaration type failure message" >&2
    cat /tmp/aether_toon_type_decl_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_PRESENCE_DECL_FAIL_FIXTURE" >/tmp/aether_toon_presence_decl_fail.out 2>&1; then
    echo "expected TOON presence declaration failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'wrongKey' must use Bool when initialized from 'toon_has_key'" /tmp/aether_toon_presence_decl_fail.out; then
    echo "missing TOON has_key declaration type failure message" >&2
    cat /tmp/aether_toon_presence_decl_fail.out >&2
    exit 1
fi
if ! grep -q "binding for 'wrongIndex' must use Bool when initialized from 'toon_has_at'" /tmp/aether_toon_presence_decl_fail.out; then
    echo "missing TOON has_at declaration type failure message" >&2
    cat /tmp/aether_toon_presence_decl_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_DEFAULTS_DECL_FAIL_FIXTURE" >/tmp/aether_toon_defaults_decl_fail.out 2>&1; then
    echo "expected TOON defaults declaration failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_get_int_or' expects a Int third argument, but 'fallbackText' is Text" /tmp/aether_toon_defaults_decl_fail.out; then
    echo "missing TOON int default fallback type failure message" >&2
    cat /tmp/aether_toon_defaults_decl_fail.out >&2
    exit 1
fi
if ! grep -q "binding for 'wrongFlag' must use Bool when initialized from 'toon_get_bool_or'" /tmp/aether_toon_defaults_decl_fail.out; then
    echo "missing TOON bool default declaration type failure message" >&2
    cat /tmp/aether_toon_defaults_decl_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$COST_ZERO_FAIL_FIXTURE" >/tmp/aether_cost_zero_fail.out 2>&1; then
    echo "expected @cost zero-budget failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "@cost budget must be greater than zero" /tmp/aether_cost_zero_fail.out; then
    echo "missing @cost zero-budget failure message" >&2
    cat /tmp/aether_cost_zero_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$COST_UNIT_FAIL_FIXTURE" >/tmp/aether_cost_unit_fail.out 2>&1; then
    echo "expected @cost unit failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "unsupported @cost unit 'ticks'" /tmp/aether_cost_unit_fail.out; then
    echo "missing @cost unit failure message" >&2
    cat /tmp/aether_cost_unit_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$COST_DETACHED_FAIL_FIXTURE" >/tmp/aether_cost_detached_fail.out 2>&1; then
    echo "expected detached @cost failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "@cost must annotate the next function declaration" /tmp/aether_cost_detached_fail.out; then
    echo "missing detached @cost failure message" >&2
    cat /tmp/aether_cost_detached_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$COST_DUPLICATE_FAIL_FIXTURE" >/tmp/aether_cost_duplicate_fail.out 2>&1; then
    echo "expected duplicate @cost failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "duplicate @cost annotation before function declaration" /tmp/aether_cost_duplicate_fail.out; then
    echo "missing duplicate @cost failure message" >&2
    cat /tmp/aether_cost_duplicate_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_PRE_EMPTY_FAIL_FIXTURE" >/tmp/aether_contract_pre_empty_fail.out 2>&1; then
    echo "expected empty @pre failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "@pre requires an expression" /tmp/aether_contract_pre_empty_fail.out; then
    echo "missing empty @pre failure message" >&2
    cat /tmp/aether_contract_pre_empty_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_POST_DETACHED_FAIL_FIXTURE" >/tmp/aether_contract_post_detached_fail.out 2>&1; then
    echo "expected detached @post failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "@post must annotate the next function declaration" /tmp/aether_contract_post_detached_fail.out; then
    echo "missing detached @post failure message" >&2
    cat /tmp/aether_contract_post_detached_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_PURE_TRAILING_FAIL_FIXTURE" >/tmp/aether_contract_pure_trailing_fail.out 2>&1; then
    echo "expected trailing @pure syntax failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "@pure does not take arguments" /tmp/aether_contract_pure_trailing_fail.out; then
    echo "missing trailing @pure syntax failure message" >&2
    cat /tmp/aether_contract_pure_trailing_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_FAIL_PRE_FIXTURE" >/tmp/aether_contract_fail_pre.out 2>&1; then
    echo "expected precondition failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether @pre failed in inc" /tmp/aether_contract_fail_pre.out; then
    echo "missing precondition failure message" >&2
    cat /tmp/aether_contract_fail_pre.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_FAIL_POST_FIXTURE" >/tmp/aether_contract_fail_post.out 2>&1; then
    echo "expected postcondition failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether @post failed in inc" /tmp/aether_contract_fail_post.out; then
    echo "missing postcondition failure message" >&2
    cat /tmp/aether_contract_fail_post.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$EFFECTS_FAIL_FIXTURE" >/tmp/aether_effects_fail.out 2>&1; then
    echo "expected effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether effect error: call to 'writeln' requires an fx block" /tmp/aether_effects_fail.out; then
    echo "missing effect-boundary failure message" >&2
    cat /tmp/aether_effects_fail.out >&2
    exit 1
fi

# The effect fence is an AST check, so token layout cannot dodge it: a call
# with its open-paren on the NEXT line (which escaped the old per-line scan)
# still needs an fx block.
if "$AETHER_BIN" --no-cache "$FX_CROSS_LINE_CALL_FAIL_FIXTURE" >/tmp/aether_fx_cross_line_call_fail.out 2>&1; then
    echo "expected cross-line effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "\[FX-001\] Aether effect error: call to 'println' requires an fx block" /tmp/aether_fx_cross_line_call_fail.out; then
    echo "missing cross-line effect-boundary failure message" >&2
    cat /tmp/aether_fx_cross_line_call_fail.out >&2
    exit 1
fi

# ...and conversely, `fx` with its `{` on the NEXT line is a valid effect
# block (the old per-line scan rejected it with a spurious FX-001).
"$AETHER_BIN" --no-cache "$FX_BRACE_NEXT_LINE_PASS_FIXTURE" >/tmp/aether_fx_brace_next_line_pass.out 2>&1
printf 'brace on next line\n' >/tmp/aether_fx_brace_next_line_pass_expected.out
if ! cmp -s /tmp/aether_fx_brace_next_line_pass_expected.out /tmp/aether_fx_brace_next_line_pass.out; then
    echo "unexpected fx-brace-on-next-line output" >&2
    cat /tmp/aether_fx_brace_next_line_pass.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TASK_ALIAS_FAIL_FIXTURE" >/tmp/aether_task_alias_fail.out 2>&1; then
    echo "expected task alias effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether effect error: call to 'task_spawn' requires an fx block" /tmp/aether_task_alias_fail.out; then
    echo "missing task alias effect-boundary failure message" >&2
    cat /tmp/aether_task_alias_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$SLEEP_ALIAS_FAIL_FIXTURE" >/tmp/aether_sleep_alias_fail.out 2>&1; then
    echo "expected sleep alias effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether effect error: call to 'sleep' requires an fx block" /tmp/aether_sleep_alias_fail.out; then
    echo "missing sleep alias effect-boundary failure message" >&2
    cat /tmp/aether_sleep_alias_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$AI_ALIAS_FAIL_FIXTURE" >/tmp/aether_ai_alias_fail.out 2>&1; then
    echo "expected ai alias effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether effect error: call to 'ai_chat' requires an fx block" /tmp/aether_ai_alias_fail.out; then
    echo "missing ai alias effect-boundary failure message" >&2
    cat /tmp/aether_ai_alias_fail.out >&2
    exit 1
fi

if [ "$HAS_OPENAI" = 1 ]; then
if env -u OPENAI_API_KEY "$AETHER_BIN" --no-cache "$RUNTIME_LINE_MAPPING_FAIL_FIXTURE" >/tmp/aether_runtime_line_mapping_fail.out 2>&1; then
    echo "expected runtime line-mapping failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "OpenAIChatCompletions requires an API key" /tmp/aether_runtime_line_mapping_fail.out; then
    echo "missing runtime openai failure message" >&2
    cat /tmp/aether_runtime_line_mapping_fail.out >&2
    exit 1
fi
# The runtime error echoes the source path the VM was handed (leading slash
# stripped); its directory is incidental to corpus location, so match the
# basename + line, not the dir prefix the umbrella baked in (Tests/aether/).
if ! grep -qE '(^|/)runtime_line_mapping_fail\.aether:4: OpenAIChatCompletions requires an API key via argument or OPENAI_API_KEY\.$' /tmp/aether_runtime_line_mapping_fail.out; then
    echo "missing plain-text runtime file/line prefix" >&2
    cat /tmp/aether_runtime_line_mapping_fail.out >&2
    exit 1
fi
if ! grep -q "\\[Error Location\\] Offset: " /tmp/aether_runtime_line_mapping_fail.out; then
    echo "missing runtime error location" >&2
    cat /tmp/aether_runtime_line_mapping_fail.out >&2
    exit 1
fi
if ! grep -q "Line: 4" /tmp/aether_runtime_line_mapping_fail.out; then
    echo "missing mapped runtime line number" >&2
    cat /tmp/aether_runtime_line_mapping_fail.out >&2
    exit 1
fi
if env -u OPENAI_API_KEY "$AETHER_BIN" --diagnostics-json --no-cache "$RUNTIME_LINE_MAPPING_FAIL_FIXTURE" >/tmp/aether_runtime_line_mapping_json.out 2>&1; then
    echo "expected runtime diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"phase":"runtime"' /tmp/aether_runtime_line_mapping_json.out; then
    echo "missing runtime diagnostics-json phase" >&2
    cat /tmp/aether_runtime_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q '"kind":"runtime"' /tmp/aether_runtime_line_mapping_json.out; then
    echo "missing runtime diagnostics-json kind" >&2
    cat /tmp/aether_runtime_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q '"file":"'"$RUNTIME_LINE_MAPPING_FAIL_FIXTURE"'"' /tmp/aether_runtime_line_mapping_json.out; then
    echo "missing runtime diagnostics-json file path" >&2
    cat /tmp/aether_runtime_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q '"line":4' /tmp/aether_runtime_line_mapping_json.out; then
    echo "missing runtime diagnostics-json line number" >&2
    cat /tmp/aether_runtime_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q 'OpenAIChatCompletions requires an API key' /tmp/aether_runtime_line_mapping_json.out; then
    echo "missing runtime diagnostics-json message" >&2
    cat /tmp/aether_runtime_line_mapping_json.out >&2
    exit 1
fi
if env -u OPENAI_API_KEY "$AETHER_BIN" --diagnostics-toon --no-cache "$RUNTIME_LINE_MAPPING_FAIL_FIXTURE" >/tmp/aether_runtime_line_mapping_toon.out 2>&1; then
    echo "expected runtime diagnostics-toon failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '^diagnostics\[1\]{severity,phase,kind,code,file,line,column,message,hint,raw}:$' /tmp/aether_runtime_line_mapping_toon.out; then
    echo "missing runtime diagnostics-toon header" >&2
    cat /tmp/aether_runtime_line_mapping_toon.out >&2
    exit 1
fi
if ! grep -q '"error","runtime","runtime","","'"$RUNTIME_LINE_MAPPING_FAIL_FIXTURE"'",4,null,"OpenAIChatCompletions requires an API key via argument or OPENAI_API_KEY\."' /tmp/aether_runtime_line_mapping_toon.out; then
    echo "missing runtime diagnostics-toon row" >&2
    cat /tmp/aether_runtime_line_mapping_toon.out >&2
    exit 1
fi
else
    echo "[skip] runtime_line_mapping_fail: OpenAI ext-builtin not present" >&2
fi

if "$AETHER_BIN" --no-cache "$PRINT_ALIAS_FAIL_FIXTURE" >/tmp/aether_print_alias_fail.out 2>&1; then
    echo "expected print alias effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether effect error: call to 'println' requires an fx block" /tmp/aether_print_alias_fail.out; then
    echo "missing print alias effect-boundary failure message" >&2
    cat /tmp/aether_print_alias_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$INFERRED_LET_UNKNOWN_FAIL_FIXTURE" >/tmp/aether_inferred_let_unknown_fail.out 2>&1; then
    echo "expected inferred let rewrite failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether declaration parser error: cannot infer the type of 'answer' from its initializer" /tmp/aether_inferred_let_unknown_fail.out; then
    echo "missing inferred let rewrite failure message" >&2
    cat /tmp/aether_inferred_let_unknown_fail.out >&2
    exit 1
fi
if ! grep -q "hint: add an explicit type, for example \`let answer: Int = ...;\`." /tmp/aether_inferred_let_unknown_fail.out; then
    echo "missing inferred let rewrite failure hint" >&2
    cat /tmp/aether_inferred_let_unknown_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$INFERRED_LET_UNKNOWN_FAIL_FIXTURE" >/tmp/aether_inferred_let_unknown_json.out 2>&1; then
    echo "expected inferred let rewrite failure with diagnostics-json but program succeeded" >&2
    exit 1
fi
if ! grep -q '"phase":"parser"' /tmp/aether_inferred_let_unknown_json.out; then
    echo "missing diagnostics-json parser phase" >&2
    cat /tmp/aether_inferred_let_unknown_json.out >&2
    exit 1
fi
if ! grep -q '"kind":"declaration"' /tmp/aether_inferred_let_unknown_json.out; then
    echo "missing diagnostics-json declaration kind" >&2
    cat /tmp/aether_inferred_let_unknown_json.out >&2
    exit 1
fi
if ! grep -q '"file":"'"$INFERRED_LET_UNKNOWN_FAIL_FIXTURE"'"' /tmp/aether_inferred_let_unknown_json.out; then
    echo "missing diagnostics-json file path" >&2
    cat /tmp/aether_inferred_let_unknown_json.out >&2
    exit 1
fi
# The guide-pointer `help: see <CODE> ...` line is folded into the preceding
# diagnostic's hint by the collector (no separate junk entry), so the hint is
# the original text plus the folded guide pointer.
if ! grep -q '"hint":"add an explicit type, for example `let answer: Int = ...;`.; see TYPE-001 in the Aether guide' /tmp/aether_inferred_let_unknown_json.out; then
    echo "missing diagnostics-json hint (with folded help pointer)" >&2
    cat /tmp/aether_inferred_let_unknown_json.out >&2
    exit 1
fi
# Function-scoped binding tables: same-name locals in different functions each
# keep their own type (Real vs Int `v`, Text vs Int `tag`) and a local that
# shadows a global const leaves the global Text binding intact for later
# functions. Under the old program-flat tables this drew a false [TYPE-001] on
# sum_real and mis-typed `let b = label;` as Int (a hard compile error).
"$AETHER_BIN" --no-cache "$SCOPED_BINDINGS_PASS_FIXTURE" >/tmp/aether_scoped_bindings_pass.out
printf '42\nanswer\n2.500000\n3\nscoped\n42\n' >/tmp/aether_scoped_bindings_expected.out
if ! cmp -s /tmp/aether_scoped_bindings_expected.out /tmp/aether_scoped_bindings_pass.out; then
    echo "unexpected scoped bindings output (regression: cross-function binding leakage / shadowed global not restored)" >&2
    cat /tmp/aether_scoped_bindings_pass.out >&2
    exit 1
fi
# The flip side: a name declared only inside another function must NOT feed
# inference. `let copy = secret;` has nothing in scope, so the parse fails with
# the coded cannot-infer diagnostic (previously the leaked Text entry typed it
# and the error surfaced later as an undefined global).
if "$AETHER_BIN" --no-cache "$SCOPED_BINDINGS_FAIL_FIXTURE" >/tmp/aether_scoped_bindings_fail.out 2>&1; then
    echo "expected scoped bindings failure but program succeeded (regression: another function's local leaked into inference)" >&2
    exit 1
fi
if ! grep -q "\[TYPE-001\] Aether declaration parser error: cannot infer the type of 'copy' from its initializer" /tmp/aether_scoped_bindings_fail.out; then
    echo "missing scoped bindings cannot-infer diagnostic" >&2
    cat /tmp/aether_scoped_bindings_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$FUNCTION_MISSING_RETURN_TYPE_FAIL_FIXTURE" >/tmp/aether_function_missing_return_type_fail.out 2>&1; then
    echo "expected missing return type rewrite failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether function parser error: functions must declare an explicit return type" /tmp/aether_function_missing_return_type_fail.out; then
    echo "missing function return type rewrite failure message" >&2
    cat /tmp/aether_function_missing_return_type_fail.out >&2
    exit 1
fi
if ! grep -q "hint: write \`fn name(args) -> Void { ... }\` or replace \`Void\` with the actual return type." /tmp/aether_function_missing_return_type_fail.out; then
    echo "missing function return type rewrite failure hint" >&2
    cat /tmp/aether_function_missing_return_type_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$FUNCTION_MISSING_RETURN_TYPE_FAIL_FIXTURE" >/tmp/aether_function_missing_return_type_json.out 2>&1; then
    echo "expected missing return type diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"phase":"parser"' /tmp/aether_function_missing_return_type_json.out; then
    echo "missing function diagnostics-json phase" >&2
    cat /tmp/aether_function_missing_return_type_json.out >&2
    exit 1
fi
if ! grep -q '"line":1' /tmp/aether_function_missing_return_type_json.out; then
    echo "missing function diagnostics-json line mapping" >&2
    cat /tmp/aether_function_missing_return_type_json.out >&2
    exit 1
fi
if ! grep -q '"code":"SYN-001"' /tmp/aether_function_missing_return_type_json.out; then
    echo "missing function diagnostics-json code" >&2
    cat /tmp/aether_function_missing_return_type_json.out >&2
    exit 1
fi
if ! grep -q '"message":"Aether function parser error: functions must declare an explicit return type\."' /tmp/aether_function_missing_return_type_json.out; then
    echo "missing function diagnostics-json message" >&2
    cat /tmp/aether_function_missing_return_type_json.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$FUNCTION_MISSING_VALUE_RETURN_FAIL_FIXTURE" >/tmp/aether_function_missing_value_return_fail.out 2>&1; then
    echo "expected missing value return rewrite failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether function parser error: non-Void functions have a fallthrough path with no return value" /tmp/aether_function_missing_value_return_fail.out; then
    echo "missing value return rewrite failure message" >&2
    cat /tmp/aether_function_missing_value_return_fail.out >&2
    exit 1
fi
if ! grep -q 'hint: add `ret value;` on the top-level path that can reach the closing `}`, or declare the function `-> Void` if it only performs side effects\.' /tmp/aether_function_missing_value_return_fail.out; then
    echo "missing value return rewrite failure hint" >&2
    cat /tmp/aether_function_missing_value_return_fail.out >&2
    exit 1
fi
# FLOW-002: an empty `ret;` in a non-Void function (a return statement exists but
# supplies no value) is a coded diagnostic, distinct from the FLOW-001 fallthrough
# rule above -- the fix differs (give the return a value vs add a return).
if "$AETHER_BIN" --no-cache "$FUNCTION_EMPTY_RETURN_FAIL_FIXTURE" >/tmp/aether_function_empty_return_fail.out 2>&1; then
    echo "expected empty-return rewrite failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether function parser error: return requires a value" /tmp/aether_function_empty_return_fail.out; then
    echo "missing empty-return rewrite failure message" >&2
    cat /tmp/aether_function_empty_return_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$FUNCTION_EMPTY_RETURN_FAIL_FIXTURE" >/tmp/aether_function_empty_return_json.out 2>&1; then
    echo "expected empty-return diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"code":"FLOW-002"' /tmp/aether_function_empty_return_json.out; then
    echo "missing empty-return diagnostics-json code FLOW-002" >&2
    cat /tmp/aether_function_empty_return_json.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$TYPE_FIELD_COMMA_FAIL_FIXTURE" >/tmp/aether_type_field_comma_fail.out 2>&1; then
    echo "expected type field comma rewrite failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether type parser error: type fields must end with ';', not ','." /tmp/aether_type_field_comma_fail.out; then
    echo "missing type field comma rewrite failure message" >&2
    cat /tmp/aether_type_field_comma_fail.out >&2
    exit 1
fi
if ! grep -q "hint: write \`fieldName: Type;\` for each field inside a \`type\` block." /tmp/aether_type_field_comma_fail.out; then
    echo "missing type field comma rewrite failure hint" >&2
    cat /tmp/aether_type_field_comma_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$TYPE_FIELD_COMMA_FAIL_FIXTURE" >/tmp/aether_type_field_comma_json.out 2>&1; then
    echo "expected type field comma diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"line":2' /tmp/aether_type_field_comma_json.out; then
    echo "missing type field comma diagnostics-json line mapping" >&2
    cat /tmp/aether_type_field_comma_json.out >&2
    exit 1
fi
if ! grep -q '"code":"SYN-001"' /tmp/aether_type_field_comma_json.out; then
    echo "missing type field comma diagnostics-json code" >&2
    cat /tmp/aether_type_field_comma_json.out >&2
    exit 1
fi
if ! grep -q '"message":"Aether type parser error: type fields must end with ' /tmp/aether_type_field_comma_json.out; then
    echo "missing type field comma diagnostics-json message" >&2
    cat /tmp/aether_type_field_comma_json.out >&2
    exit 1
fi
# Constant record-field defaults (`field: Type = <const>`). The positive fixture
# exercises Int/Real/Text/Bool defaults, a construction-site override (which must
# win over the default), and an unset field keeping its default -- including a
# string override longer than the default (regression: a fixed-capacity default
# must not clamp the field) and an Int default in a Real field (int->real widen).
# The negatives are the FIELD-003 constant boundary and the TYPE-001 type
# mismatch, both coded so --diagnostics-json feeds the guide map.
"$AETHER_BIN" --no-cache "$TYPE_FIELD_DEFAULT_PASS_FIXTURE" >/tmp/aether_type_field_default_pass.out
printf '3\ndef\n9\nover\n1.500000\non\n' >/tmp/aether_type_field_default_expected.out
if ! cmp -s /tmp/aether_type_field_default_expected.out /tmp/aether_type_field_default_pass.out; then
    echo "unexpected constant field default output (regression: defaults / override / unset / capacity)" >&2
    cat /tmp/aether_type_field_default_pass.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$TYPE_FIELD_DEFAULT_NONCONST_FAIL_FIXTURE" >/tmp/aether_type_field_default_nonconst_fail.out 2>&1; then
    echo "expected non-constant field default failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "only constant field defaults are supported" /tmp/aether_type_field_default_nonconst_fail.out; then
    echo "missing non-constant field default failure message" >&2
    cat /tmp/aether_type_field_default_nonconst_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$TYPE_FIELD_DEFAULT_NONCONST_FAIL_FIXTURE" >/tmp/aether_type_field_default_nonconst_json.out 2>&1; then
    echo "expected non-constant field default diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"code":"FIELD-003"' /tmp/aether_type_field_default_nonconst_json.out; then
    echo "missing non-constant field default diagnostics-json code FIELD-003" >&2
    cat /tmp/aether_type_field_default_nonconst_json.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$TYPE_FIELD_DEFAULT_TYPE_MISMATCH_FAIL_FIXTURE" >/tmp/aether_type_field_default_type_mismatch_fail.out 2>&1; then
    echo "expected field default type-mismatch failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "field default value type mismatch" /tmp/aether_type_field_default_type_mismatch_fail.out; then
    echo "missing field default type-mismatch failure message" >&2
    cat /tmp/aether_type_field_default_type_mismatch_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$TYPE_FIELD_DEFAULT_TYPE_MISMATCH_FAIL_FIXTURE" >/tmp/aether_type_field_default_type_mismatch_json.out 2>&1; then
    echo "expected field default type-mismatch diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"code":"TYPE-001"' /tmp/aether_type_field_default_type_mismatch_json.out; then
    echo "missing field default type-mismatch diagnostics-json code TYPE-001" >&2
    cat /tmp/aether_type_field_default_type_mismatch_json.out >&2
    exit 1
fi
# Reserved-word collisions (broadest generative-testing gap): a field or method
# named after a reserved word/type name/operator word must name the collision,
# not the bare "unexpected token in type body" / "expected function name" errors.
if "$AETHER_BIN" --no-cache "$RESERVED_FIELD_NAME_FAIL_FIXTURE" >/tmp/aether_reserved_field.out 2>&1; then
    echo "expected reserved field-name failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "'word' is a reserved type name and cannot be used as a field name." /tmp/aether_reserved_field.out; then
    echo "missing reserved field-name collision message" >&2
    cat /tmp/aether_reserved_field.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$RESERVED_FIELD_NAME_FAIL_FIXTURE" >/tmp/aether_reserved_field_json.out 2>&1; then
    echo "expected reserved field-name diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"line":2' /tmp/aether_reserved_field_json.out; then
    echo "missing reserved field-name diagnostics-json line mapping" >&2
    cat /tmp/aether_reserved_field_json.out >&2
    exit 1
fi
if ! grep -q '"code":"SYN-001"' /tmp/aether_reserved_field_json.out; then
    echo "missing reserved field-name diagnostics-json code" >&2
    cat /tmp/aether_reserved_field_json.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$RESERVED_METHOD_NAME_FAIL_FIXTURE" >/tmp/aether_reserved_method.out 2>&1; then
    echo "expected reserved method-name failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "'mul' is a reserved operator word and cannot be used as a method name." /tmp/aether_reserved_method.out; then
    echo "missing reserved method-name collision message" >&2
    cat /tmp/aether_reserved_method.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$RESERVED_METHOD_NAME_FAIL_FIXTURE" >/tmp/aether_reserved_method_json.out 2>&1; then
    echo "expected reserved method-name diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"line":3' /tmp/aether_reserved_method_json.out; then
    echo "missing reserved method-name diagnostics-json line mapping" >&2
    cat /tmp/aether_reserved_method_json.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$RESERVED_NEW_METHOD_FAIL_FIXTURE" >/tmp/aether_reserved_new.out 2>&1; then
    echo "expected reserved new-method failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "'new' is a reserved keyword (the object allocator) and cannot be used as a method name." /tmp/aether_reserved_new.out; then
    echo "missing reserved new-method collision message" >&2
    cat /tmp/aether_reserved_new.out >&2
    exit 1
fi
if ! grep -q "Aether has no constructor methods" /tmp/aether_reserved_new.out; then
    echo "missing reserved new-method constructor hint" >&2
    cat /tmp/aether_reserved_new.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE" >/tmp/aether_diagnostic_line_mapping_json.out 2>&1; then
    echo "expected diagnostic line-mapping failure with diagnostics-json but program succeeded" >&2
    exit 1
fi
if ! grep -q '"file":"'"$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE"'"' /tmp/aether_diagnostic_line_mapping_json.out; then
    echo "missing mapped diagnostics-json file path" >&2
    cat /tmp/aether_diagnostic_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q '"line":8' /tmp/aether_diagnostic_line_mapping_json.out; then
    echo "missing mapped diagnostics-json loop line" >&2
    cat /tmp/aether_diagnostic_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q '"line":9' /tmp/aether_diagnostic_line_mapping_json.out; then
    echo "missing mapped diagnostics-json body line" >&2
    cat /tmp/aether_diagnostic_line_mapping_json.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-toon --no-cache "$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE" >/tmp/aether_diagnostic_line_mapping_toon.out 2>&1; then
    echo "expected diagnostic line-mapping failure with diagnostics-toon but program succeeded" >&2
    exit 1
fi
if ! grep -q '^diagnostics\[3\]{severity,phase,kind,code,file,line,column,message,hint,raw}:$' /tmp/aether_diagnostic_line_mapping_toon.out; then
    echo "missing diagnostics-toon header" >&2
    cat /tmp/aether_diagnostic_line_mapping_toon.out >&2
    exit 1
fi
if ! grep -q '"scope","SCOPE-001","'"$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE"'".*,8,null,"identifier '\''i'\'' not in scope\."' /tmp/aether_diagnostic_line_mapping_toon.out; then
    echo "missing diagnostics-toon mapped loop line" >&2
    cat /tmp/aether_diagnostic_line_mapping_toon.out >&2
    exit 1
fi
if ! grep -q '"scope","SCOPE-001","'"$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE"'".*,9,null,"identifier '\''i'\'' not in scope\."' /tmp/aether_diagnostic_line_mapping_toon.out; then
    echo "missing diagnostics-toon mapped body line" >&2
    cat /tmp/aether_diagnostic_line_mapping_toon.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$PURE_FAIL_EFFECTFUL_FIXTURE" >/tmp/aether_pure_fail_effectful.out 2>&1; then
    echo "expected purity failure for effectful builtin but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether purity error: pure function 'noisy' cannot call effectful builtin 'writeln'" /tmp/aether_pure_fail_effectful.out; then
    echo "missing purity failure for effectful builtin" >&2
    cat /tmp/aether_pure_fail_effectful.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$PURE_FAIL_NON_PURE_CALL_FIXTURE" >/tmp/aether_pure_fail_non_pure_call.out 2>&1; then
    echo "expected purity failure for non-pure call but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether purity error: pure function 'wrapper' cannot call non-pure function 'noisy'" /tmp/aether_pure_fail_non_pure_call.out; then
    echo "missing purity failure for non-pure call" >&2
    cat /tmp/aether_pure_fail_non_pure_call.out >&2
    exit 1
fi

# @pure functions may not contain fx blocks at all (guide rule): the block
# itself is rejected (ANN-001), independent of what it calls.
if "$AETHER_BIN" --no-cache "$PURE_CONTAINS_FX_FAIL_FIXTURE" >/tmp/aether_pure_contains_fx_fail.out 2>&1; then
    echo "expected purity failure for fx block in pure function but program succeeded" >&2
    exit 1
fi
if ! grep -q "\[ANN-001\] Aether purity error: pure function 'shout' contains an fx block" /tmp/aether_pure_contains_fx_fail.out; then
    echo "missing purity failure for fx block in pure function" >&2
    cat /tmp/aether_pure_contains_fx_fail.out >&2
    exit 1
fi

if ! "$AETHER_BIN" --no-cache "$IMPORT_MISSING_FAIL_FIXTURE" >/tmp/aether_import_missing_fail.out 2>&1; then
    echo "missing import fixture should succeed by default" >&2
    cat /tmp/aether_import_missing_fail.out >&2
    exit 1
fi
if [ -s /tmp/aether_import_missing_fail.out ]; then
    echo "default missing import run should be silent" >&2
    cat /tmp/aether_import_missing_fail.out >&2
    exit 1
fi
if ! "$AETHER_BIN" --no-cache --verbose-compat "$IMPORT_MISSING_FAIL_FIXTURE" >/tmp/aether_import_missing_verbose.out 2>&1; then
    echo "missing import verbose-compat run should still succeed" >&2
    cat /tmp/aether_import_missing_verbose.out >&2
    exit 1
fi
if ! grep -q "^$IMPORT_MISSING_FAIL_FIXTURE:1: warning: \[IMP-001\] Aether ignored missing import 'definitely_missing_aether_module'\.$" /tmp/aether_import_missing_verbose.out; then
    echo "missing verbose-compat warning for ignored import" >&2
    cat /tmp/aether_import_missing_verbose.out >&2
    exit 1
fi
if ! "$AETHER_BIN" --no-cache --diagnostics-json "$IMPORT_MISSING_FAIL_FIXTURE" >/tmp/aether_import_missing_json.out 2>&1; then
    echo "missing import diagnostics-json run should succeed" >&2
    cat /tmp/aether_import_missing_json.out >&2
    exit 1
fi
if [ -s /tmp/aether_import_missing_json.out ]; then
    echo "missing import diagnostics-json output should stay empty" >&2
    cat /tmp/aether_import_missing_json.out >&2
    exit 1
fi
if ! "$AETHER_BIN" --no-cache --diagnostics-toon "$IMPORT_MISSING_FAIL_FIXTURE" >/tmp/aether_import_missing_toon.out 2>&1; then
    echo "missing import diagnostics-toon run should succeed" >&2
    cat /tmp/aether_import_missing_toon.out >&2
    exit 1
fi
if [ -s /tmp/aether_import_missing_toon.out ]; then
    echo "missing import diagnostics-toon output should stay empty" >&2
    cat /tmp/aether_import_missing_toon.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$PAR_FAIL_NON_CALL_FIXTURE" >/tmp/aether_par_fail_non_call.out 2>&1; then
    echo "expected par rewrite failure for non-call statement but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether par parser error: only direct call statements are allowed inside par blocks" /tmp/aether_par_fail_non_call.out; then
    echo "missing par rewrite failure message" >&2
    cat /tmp/aether_par_fail_non_call.out >&2
    exit 1
fi
# PAR-002: the par-arity rule (only direct call statements inside par) is coded,
# distinct from the PAR-001 shared-record data race below.
if "$AETHER_BIN" --diagnostics-json --no-cache "$PAR_FAIL_NON_CALL_FIXTURE" >/tmp/aether_par_fail_non_call_json.out 2>&1; then
    echo "expected par non-call diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"code":"PAR-002"' /tmp/aether_par_fail_non_call_json.out; then
    echo "missing par non-call diagnostics-json code PAR-002" >&2
    cat /tmp/aether_par_fail_non_call_json.out >&2
    exit 1
fi
# FIELD-002 backstop: a method call on a receiver of an unresolved type lowers to a
# backend field lookup that emits the raw, uncoded "Compiler error: Unknown field
# 'T.m'." at codegen -- past rea's semantic FIELD-002 check. The --diagnostics-json
# collector must backfill the code via frontend inference so the repair loop sees
# FIELD-002 rather than code:null.
if "$AETHER_BIN" --diagnostics-json --no-cache "$BACKEND_UNKNOWN_FIELD_CODED_FAIL_FIXTURE" >/tmp/aether_backend_unknown_field_json.out 2>&1; then
    echo "expected backend unknown-field diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"code":"FIELD-002"' /tmp/aether_backend_unknown_field_json.out; then
    echo "missing backend unknown-field diagnostics-json code FIELD-002 (collector backstop regressed?)" >&2
    cat /tmp/aether_backend_unknown_field_json.out >&2
    exit 1
fi

# PAR-001: the same pointer-backed record passed to more than one par branch is a
# concurrent double-free at runtime (SIGABRT/SIGTRAP), silent today. It must be a
# compile-time diagnostic, not a crash.
if "$AETHER_BIN" --no-cache "$PAR_SHARED_RECORD_FAIL_FIXTURE" >/tmp/aether_par_shared_record_fail.out 2>&1; then
    echo "expected PAR-001 for a record shared across par branches but program succeeded" >&2
    exit 1
fi
if ! grep -q "\[PAR-001\] Aether par error: record 'a' is shared by more than one par branch" /tmp/aether_par_shared_record_fail.out; then
    echo "missing PAR-001 shared-record diagnostic" >&2
    cat /tmp/aether_par_shared_record_fail.out >&2
    exit 1
fi

# Two par branches calling the SAME tuple-returning function used to race on
# shared __aether_tuple_N_itemK globals (PAR-003, rejected at compile time).
# Tuple returns now lower to a record returned by value (VM deep-copies on
# every return), so each spawned call gets its own independent result and
# there is nothing left to race -- this must compile and run successfully.
# par branches only support bare direct-call statements (parseParBlock), so a
# spawned call's result can't be captured/joined here; this is a compile+run
# smoke test, not an output check (see par_shared_tuple_call_pass.aether).
if ! "$AETHER_BIN" --no-cache "$PAR_SHARED_TUPLE_CALL_PASS_FIXTURE" >/tmp/aether_par_shared_tuple_call_pass.out 2>&1; then
    echo "expected par-shared-tuple-call to compile and run successfully" >&2
    cat /tmp/aether_par_shared_tuple_call_pass.out >&2
    exit 1
fi

# SCOPE-001: calling a method that is not defined on a record must fail at compile
# time (parser lowers recv.method() to a Type.method global; aetherCheckMemberCalls
# verifies it exists) rather than as a late "Undefined global variable" at runtime.
if "$AETHER_BIN" --no-cache "$METHOD_UNDEFINED_FAIL_FIXTURE" >/tmp/aether_method_undefined_fail.out 2>&1; then
    echo "expected SCOPE-001 for an undefined method but program succeeded" >&2
    exit 1
fi
if ! grep -q "\[SCOPE-001\] Aether method error: method 'distance' is not defined on type 'Point'" /tmp/aether_method_undefined_fail.out; then
    echo "missing SCOPE-001 undefined-method diagnostic" >&2
    cat /tmp/aether_method_undefined_fail.out >&2
    exit 1
fi

# SYN-001 backstop: an unparseable top-level construct must never exit non-zero
# with empty stderr (the worst case for the repair loop). The silent-failure
# backstop guarantees a coded diagnostic anchored where parsing stalled.
if "$AETHER_BIN" --no-cache "$UNKNOWN_CONSTRUCT_FAIL_FIXTURE" >/tmp/aether_unknown_construct_fail.out 2>&1; then
    echo "expected SYN-001 for an unparseable construct but program succeeded" >&2
    exit 1
fi
# Accept either the parser's own anchored diagnostic ("Aether parser rewrite
# error", emitted since the block parser stopped tolerating stalled statements)
# or the silent-failure backstop's wording ("Aether syntax error"); both are
# coded SYN-001 and anchored where parsing stalled.
if ! grep -q "\[SYN-001\] Aether \(syntax\|parser\) error:" /tmp/aether_unknown_construct_fail.out; then
    echo "missing SYN-001 backstop diagnostic (silent parse failure regressed)" >&2
    cat /tmp/aether_unknown_construct_fail.out >&2
    exit 1
fi
if ! [ -s /tmp/aether_unknown_construct_fail.out ]; then
    echo "backstop produced empty output for an unparseable construct" >&2
    exit 1
fi

# Missing closing delimiters are hard SYN-001 errors, never silently tolerated
# (regression: parseBlock/parseArgListEx used to consume-if-present, so an
# unclosed body at EOF or an unclosed call arg list parsed without error).
if "$AETHER_BIN" --no-cache "$UNCLOSED_BLOCK_FAIL_FIXTURE" >/tmp/aether_unclosed_block_fail.out 2>&1; then
    echo "expected unclosed-block failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "\[SYN-001\].*expected '}' to close block (opened at line 3)" /tmp/aether_unclosed_block_fail.out; then
    echo "missing unclosed-block SYN-001 diagnostic" >&2
    cat /tmp/aether_unclosed_block_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --no-cache "$UNCLOSED_CALL_FAIL_FIXTURE" >/tmp/aether_unclosed_call_fail.out 2>&1; then
    echo "expected unclosed-call failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "\[SYN-001\].*expected ')' to close argument list" /tmp/aether_unclosed_call_fail.out; then
    echo "missing unclosed-call SYN-001 diagnostic" >&2
    cat /tmp/aether_unclosed_call_fail.out >&2
    exit 1
fi

# A range-loop bound like `0..5 && !stop` used to be parsed by the full
# expression grammar (parseExpr), which doesn't stop at `&&`/`||`, so the
# whole `5 && !stop` silently became the upper bound -- a Bool that coerced
# to 0/1, running the loop once instead of erroring or iterating as intended.
# Bounds are now parsed at additive precedence (parseAdd), so this must be a
# hard parser error (existing "expected '{' to open loop body" path, since
# parseAdd correctly stops at `5` and leaves `&&` unconsumed).
if "$AETHER_BIN" --no-cache "$LOOP_RANGE_BOOL_BOUND_FAIL_FIXTURE" >/tmp/aether_loop_range_bool_bound_fail.out 2>&1; then
    echo "expected loop-range Bool-bound failure but program succeeded" >&2
    cat /tmp/aether_loop_range_bool_bound_fail.out >&2
    exit 1
fi
if ! grep -q "\[SYN-001\].*expected '{' to open loop body" /tmp/aether_loop_range_bool_bound_fail.out; then
    echo "missing loop-range Bool-bound SYN-001 diagnostic" >&2
    cat /tmp/aether_loop_range_bool_bound_fail.out >&2
    exit 1
fi
# Fixed-size array types (Int[3]) are a single clear SYN-001 with the Int[]
# hint, and the type parser resyncs past the ']' so no unrelated downstream
# diagnostic appears (regression: consumed '[' then abandoned the stream).
if "$AETHER_BIN" --no-cache "$FIXED_SIZE_ARRAY_FAIL_FIXTURE" >/tmp/aether_fixed_size_array_fail.out 2>&1; then
    echo "expected fixed-size array failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "\[SYN-001\].*fixed-size array types are not supported" /tmp/aether_fixed_size_array_fail.out; then
    echo "missing fixed-size array SYN-001 diagnostic" >&2
    cat /tmp/aether_fixed_size_array_fail.out >&2
    exit 1
fi
if ! grep -q "hint: use \`Int\[\]\`" /tmp/aether_fixed_size_array_fail.out; then
    echo "missing fixed-size array dynamic-array hint" >&2
    cat /tmp/aether_fixed_size_array_fail.out >&2
    exit 1
fi
if [ "$(grep -c "\[SYN-001\]" /tmp/aether_fixed_size_array_fail.out)" != "1" ]; then
    echo "fixed-size array should produce exactly one SYN-001 (stream resync regressed)" >&2
    cat /tmp/aether_fixed_size_array_fail.out >&2
    exit 1
fi
# A print format spec whose ':' is not followed by a number is SYN-001
# (regression: parseWriteArg swallowed the ':' and misaligned the stream).
if "$AETHER_BIN" --no-cache "$WRITE_FORMAT_COLON_FAIL_FIXTURE" >/tmp/aether_write_format_colon_fail.out 2>&1; then
    echo "expected write-format colon failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "\[SYN-001\].*expected a number after ':' in print format spec" /tmp/aether_write_format_colon_fail.out; then
    echo "missing write-format colon SYN-001 diagnostic" >&2
    cat /tmp/aether_write_format_colon_fail.out >&2
    exit 1
fi
# Direct recursion in a tuple-return fn used to be rejected at compile time
# (TUP-001: tuple returns lowered to per-slot globals, so a self-call
# corrupted its own results). Tuple returns now lower to a record returned by
# value (VM deep-copies on every return -- returnFromCall/copyRecord in
# pscal-core), so each call frame gets its own independent result: this now
# compiles and produces the correct accumulated values.
"$AETHER_BIN" --no-cache "$TUPLE_RECURSION_PASS_FIXTURE" >/tmp/aether_tuple_recursion_pass.out 2>&1
printf '3\n6\n' >/tmp/aether_tuple_recursion_expected.out
if ! cmp -s /tmp/aether_tuple_recursion_expected.out /tmp/aether_tuple_recursion_pass.out; then
    echo "unexpected tuple-recursion output" >&2
    cat /tmp/aether_tuple_recursion_pass.out >&2
    exit 1
fi
# Indirect recursion through tuple-returning functions (a() calls b() calls
# a()) is the same defect class one hop removed; same reentrant record-return
# fix applies, so this also now compiles and produces correct results.
"$AETHER_BIN" --no-cache "$TUPLE_INDIRECT_RECURSION_PASS_FIXTURE" >/tmp/aether_tuple_indirect_recursion_pass.out 2>&1
printf '2\n4\n' >/tmp/aether_tuple_indirect_recursion_expected.out
if ! cmp -s /tmp/aether_tuple_indirect_recursion_expected.out /tmp/aether_tuple_indirect_recursion_pass.out; then
    echo "unexpected indirect tuple-recursion output" >&2
    cat /tmp/aether_tuple_indirect_recursion_pass.out >&2
    exit 1
fi
# ...while plain (non-tuple) direct recursion still parses and runs.
"$AETHER_BIN" --no-cache "$RECURSION_PASS_FIXTURE" >/tmp/aether_recursion_pass.out 2>&1
if ! grep -qx "120" /tmp/aether_recursion_pass.out; then
    echo "unexpected non-tuple recursion output" >&2
    cat /tmp/aether_recursion_pass.out >&2
    exit 1
fi
# Recursion through an Int/Int division (which boxes a Double at the
# division site, then retypes to the callee's Int64 param) must not crash --
# regression for the setTypeValue stale-box-bits bug fixed in pscal-core#6.
"$AETHER_BIN" --no-cache "$RET_RECURSION_PASS_FIXTURE" >/tmp/aether_ret_recursion_pass.out 2>&1
if ! grep -qx "15" /tmp/aether_ret_recursion_pass.out; then
    echo "unexpected ret-recursion (digit sum) output" >&2
    cat /tmp/aether_ret_recursion_pass.out >&2
    exit 1
fi
# `ret expr` must coerce to the declared return type like every other typed
# sink: a `-> Int` body ending in Int/Int division returns 24, not 24.000000,
# and a `-> Real` body returning an Int promotes to 5.000000 -- regression
# for the returnFromCall coercion gap fixed in pscal-core.
"$AETHER_BIN" --no-cache "$RET_INT_DIVISION_PASS_FIXTURE" >/tmp/aether_ret_int_division_pass.out 2>&1
printf 'mean = 24\npromoted = 5.000000\n' >/tmp/aether_ret_int_division_expected.out
if ! cmp -s /tmp/aether_ret_int_division_expected.out /tmp/aether_ret_int_division_pass.out; then
    echo "unexpected ret int-division coercion output" >&2
    cat /tmp/aether_ret_int_division_pass.out >&2
    exit 1
fi

"$AETHER_BIN" --no-cache "$TOON_COMMENT_ARITH_PASS_FIXTURE" >/tmp/aether_toon_comment_arith_pass.out
if ! grep -q '^Ada$' /tmp/aether_toon_comment_arith_pass.out; then
    echo "missing TOON comment arithmetic pass output" >&2
    cat /tmp/aether_toon_comment_arith_pass.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_OBJECT_ROOT_ITER_FAIL_FIXTURE" >/tmp/aether_toon_object_root_iter_fail.out 2>&1; then
    echo "expected object-root iteration failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '\[AETH-RUNTIME-TOON-GET-INDEX-ARRAY\]' /tmp/aether_toon_object_root_iter_fail.out; then
    echo "missing object-root iteration runtime code" >&2
    cat /tmp/aether_toon_object_root_iter_fail.out >&2
    exit 1
fi
if ! grep -q 'YyjsonGetIndex requires an array value handle, got object\.' /tmp/aether_toon_object_root_iter_fail.out; then
    echo "missing object-root iteration type-aware runtime error" >&2
    cat /tmp/aether_toon_object_root_iter_fail.out >&2
    exit 1
fi
if ! grep -q 'extract its array field first, for example toon_key(root, "jobs")' /tmp/aether_toon_object_root_iter_fail.out; then
    echo "missing object-root iteration hint" >&2
    cat /tmp/aether_toon_object_root_iter_fail.out >&2
    exit 1
fi

"$AETHER_BIN" --no-cache "$TOON_NESTED_HELPERS_PASS_FIXTURE" >/tmp/aether_toon_nested_helpers_pass.out
if ! grep -q '^Ada 91$' /tmp/aether_toon_nested_helpers_pass.out; then
    echo "missing TOON nested helper pass output" >&2
    cat /tmp/aether_toon_nested_helpers_pass.out >&2
    exit 1
fi

# MStream is a first-class opaque handle type (MS-001): declarations,
# inference, and mstreambuffer -> Text extraction must run end to end.
"$AETHER_BIN" --no-cache "$MSTREAM_HANDLE_PASS_FIXTURE" >/tmp/aether_mstream_handle_pass.out
printf 'aether streams\nempty ok\n' >/tmp/aether_mstream_handle_expected.out
if ! cmp -s /tmp/aether_mstream_handle_expected.out /tmp/aether_mstream_handle_pass.out; then
    echo "unexpected MStream handle pass output" >&2
    cat /tmp/aether_mstream_handle_pass.out >&2
    exit 1
fi

# `let stream: Int = mstreamfromstring(...)` must be a COMPILE-time MS-001,
# not the runtime VM crash "Cannot assign MEMORY_STREAM to integer".
if "$AETHER_BIN" --no-cache "$MSTREAM_DECL_INT_FAIL_FIXTURE" >/tmp/aether_mstream_decl_int_fail.out 2>&1; then
    echo "expected MStream Int-declaration failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'stream' must use MStream when initialized from 'mstreamfromstring'" /tmp/aether_mstream_decl_int_fail.out; then
    echo "missing MStream Int-declaration failure message" >&2
    cat /tmp/aether_mstream_decl_int_fail.out >&2
    exit 1
fi
if ! grep -q "MS-001" /tmp/aether_mstream_decl_int_fail.out; then
    echo "MStream Int-declaration diagnostic missing MS-001 code" >&2
    cat /tmp/aether_mstream_decl_int_fail.out >&2
    exit 1
fi
if grep -q "Cannot assign MEMORY_STREAM to integer" /tmp/aether_mstream_decl_int_fail.out; then
    echo "MStream Int-declaration crashed at runtime instead of failing at compile time" >&2
    cat /tmp/aether_mstream_decl_int_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$MSTREAM_HANDLE_ARITH_FAIL_FIXTURE" >/tmp/aether_mstream_handle_arith_fail.out 2>&1; then
    echo "expected MStream arithmetic failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "opaque MStream handle 's' cannot be used in arithmetic expressions" /tmp/aether_mstream_handle_arith_fail.out; then
    echo "missing MStream arithmetic failure message" >&2
    cat /tmp/aether_mstream_handle_arith_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$MSTREAM_CROSS_KIND_FAIL_FIXTURE" >/tmp/aether_mstream_cross_kind_fail.out 2>&1; then
    echo "expected MStream cross-kind declaration failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'doc' must use MStream when initialized from 'mstreamcreate'" /tmp/aether_mstream_cross_kind_fail.out; then
    echo "missing MStream cross-kind declaration failure message" >&2
    cat /tmp/aether_mstream_cross_kind_fail.out >&2
    exit 1
fi

# Networking stays fx-gated: httpsession() outside fx is FX-001.
if "$AETHER_BIN" --no-cache "$HTTP_SESSION_FX_FAIL_FIXTURE" >/tmp/aether_http_session_fx_fail.out 2>&1; then
    echo "expected httpsession fx failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'httpsession' requires an fx block" /tmp/aether_http_session_fx_fail.out; then
    echo "missing httpsession fx failure message" >&2
    cat /tmp/aether_http_session_fx_fail.out >&2
    exit 1
fi
if ! grep -q "FX-001" /tmp/aether_http_session_fx_fail.out; then
    echo "httpsession fx diagnostic missing FX-001 code" >&2
    cat /tmp/aether_http_session_fx_fail.out >&2
    exit 1
fi

# A user-declared top-level `fn swap` shadows the same-named, effectful PSCAL
# vm_builtin for FX-001 purposes: calling the user's OWN swap from outside any
# fx block must compile and run (previously misfired FX-001 on name alone).
"$AETHER_BIN" --no-cache "$SWAP_SHADOW_BUILTIN_PASS_FIXTURE" >/tmp/aether_swap_shadow_builtin_pass.out
printf '12345\n' >/tmp/aether_swap_shadow_builtin_expected.out
if ! cmp -s /tmp/aether_swap_shadow_builtin_expected.out /tmp/aether_swap_shadow_builtin_pass.out; then
    echo "unexpected swap-shadows-builtin bubble sort output" >&2
    cat /tmp/aether_swap_shadow_builtin_pass.out >&2
    exit 1
fi

# Without a user-declared `swap`, the real vm_builtin still requires fx: the
# shadowing fix must not blanket-suppress FX-001 for the builtin itself.
if "$AETHER_BIN" --no-cache "$SWAP_BUILTIN_UNSHADOWED_FAIL_FIXTURE" >/tmp/aether_swap_builtin_unshadowed_fail.out 2>&1; then
    echo "expected swap builtin fx failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'swap' requires an fx block" /tmp/aether_swap_builtin_unshadowed_fail.out; then
    echo "missing swap builtin fx failure message" >&2
    cat /tmp/aether_swap_builtin_unshadowed_fail.out >&2
    exit 1
fi
if ! grep -q "FX-001" /tmp/aether_swap_builtin_unshadowed_fail.out; then
    echo "swap builtin fx diagnostic missing FX-001 code" >&2
    cat /tmp/aether_swap_builtin_unshadowed_fail.out >&2
    exit 1
fi

# The documented HTTP + MStream shape must COMPILE on every build (curl-less
# builds fail only at runtime, so this is a --no-run check).
"$AETHER_BIN" --no-cache --no-run "$HTTP_MSTREAM_COMPILE_PASS_FIXTURE" >/dev/null

"$AETHER_BIN" --no-cache "$TOON_SINGLE_CHAR_KEY_PASS_FIXTURE" >/tmp/aether_toon_single_char_key_pass.out
if grep -qx "yyjson unavailable" /tmp/aether_toon_single_char_key_pass.out; then
    :
else
    printf 'true\nfalse\n3.5\n0.0\n' >/tmp/aether_toon_single_char_key_expected.out
    if ! cmp -s /tmp/aether_toon_single_char_key_expected.out /tmp/aether_toon_single_char_key_pass.out; then
        echo "unexpected TOON single-char key output" >&2
        cat /tmp/aether_toon_single_char_key_pass.out >&2
        exit 1
    fi
fi

# --- One-liner block expansion (SYN-001 fix): `if c { fx {...} ret; }` and
#     friends must parse and behave exactly like their multi-line form. ---
cat > /tmp/aether_oneliner_guard.aether <<'AETH'
fn main() -> Void {
    let x: Int = 5;
    if x > 3 { fx { println("big"); } ret; }
    fx { println("end"); }
    ret;
}
AETH
"$AETHER_BIN" --no-cache /tmp/aether_oneliner_guard.aether >/tmp/aether_oneliner_guard.out 2>&1
printf 'big\n' >/tmp/aether_oneliner_guard_expected.out
if ! cmp -s /tmp/aether_oneliner_guard_expected.out /tmp/aether_oneliner_guard.out; then
    echo "unexpected one-liner guard output" >&2
    cat /tmp/aether_oneliner_guard.out >&2
    exit 1
fi

cat > /tmp/aether_oneliner_ifelse.aether <<'AETH'
fn main() -> Void {
    let x: Int = 1;
    if x > 3 { fx { println("big"); } } else { fx { println("small"); } }
    ret;
}
AETH
"$AETHER_BIN" --no-cache /tmp/aether_oneliner_ifelse.aether >/tmp/aether_oneliner_ifelse.out 2>&1
printf 'small\n' >/tmp/aether_oneliner_ifelse_expected.out
if ! cmp -s /tmp/aether_oneliner_ifelse_expected.out /tmp/aether_oneliner_ifelse.out; then
    echo "unexpected one-liner if/else output" >&2
    cat /tmp/aether_oneliner_ifelse.out >&2
    exit 1
fi

# Error reported on a line *after* a one-liner must keep the original line
# number (the expansion maps every produced line back to the source line).
cat > /tmp/aether_oneliner_linemap.aether <<'AETH'
fn main() -> Void {
    let x: Int = 5;
    if x > 3 { ret; }
    writeln("oops");
    ret;
}
AETH
"$AETHER_BIN" --no-cache /tmp/aether_oneliner_linemap.aether >/tmp/aether_oneliner_linemap.out 2>&1 || true
if ! grep -q ':4: \[FX-001\]' /tmp/aether_oneliner_linemap.out; then
    echo "one-liner expansion shifted diagnostic line numbers" >&2
    cat /tmp/aether_oneliner_linemap.out >&2
    exit 1
fi

# One-liner condition with a toon_* capability call must still resolve.
cat > /tmp/aether_oneliner_toon.aether <<'AETH'
fn main() -> Void {
    if !has_toon() {
        fx { println("yyjson unavailable"); }
        ret;
    }
    let doc: ToonDoc = toon_parse("{\"v\":7}");
    let root: ToonNode = toon_root(doc);
    let item: ToonNode = toon_key(root, "v");
    let kind: Text = "other";
    if toon_is_int(item) { kind = "int"; }
    fx { println(kind); }
    toon_close(doc);
    ret;
}
AETH
"$AETHER_BIN" --no-cache /tmp/aether_oneliner_toon.aether >/tmp/aether_oneliner_toon.out 2>&1
if grep -qx "yyjson unavailable" /tmp/aether_oneliner_toon.out; then
    :
else
    printf 'int\n' >/tmp/aether_oneliner_toon_expected.out
    if ! cmp -s /tmp/aether_oneliner_toon_expected.out /tmp/aether_oneliner_toon.out; then
        echo "unexpected one-liner toon-condition output" >&2
        cat /tmp/aether_oneliner_toon.out >&2
        exit 1
    fi
fi

# One-liner body statements must reach the SAME specialized per-statement
# handlers the main rewrite loop applies, so a body needing one lowers exactly
# like its multi-line form. A tuple `ret (a, b);` inside a one-liner guard must
# hit translateTupleReturnLine (bare translateLine leaks `return (a, b);` ->
# SYN-001 Unexpected COMMA). The minmax(3, 7) call takes the one-liner branch.
cat > /tmp/aether_oneliner_tuple_ret.aether <<'AETH'
fn minmax(a: Int, b: Int) -> (Int, Int) {
    if a <= b { ret (a, b); }
    ret (b, a);
}
fn main() -> Void {
    let (lo, hi) = minmax(3, 7);
    fx {
        println(lo);
        println(hi);
    }
    ret;
}
AETH
"$AETHER_BIN" --no-cache /tmp/aether_oneliner_tuple_ret.aether >/tmp/aether_oneliner_tuple_ret.out 2>&1
printf '3\n7\n' >/tmp/aether_oneliner_tuple_ret_expected.out
if ! cmp -s /tmp/aether_oneliner_tuple_ret_expected.out /tmp/aether_oneliner_tuple_ret.out; then
    echo "unexpected one-liner tuple-return output" >&2
    cat /tmp/aether_oneliner_tuple_ret.out >&2
    exit 1
fi

# An array append (`xs = xs + [v];`) inside a one-liner loop body must hit
# translateArrayAppendLine, not bare translateLine (which leaks ARRAY + ARRAY ->
# runtime "Operands must be numbers").
cat > /tmp/aether_oneliner_array_append.aether <<'AETH'
fn main() -> Void {
    let squares: Int[] = [];
    loop i in 0..4 { squares = squares + [i * i]; }
    let count: Int = length(squares);
    fx { println("count = ", count); }
    ret;
}
AETH
"$AETHER_BIN" --no-cache /tmp/aether_oneliner_array_append.aether >/tmp/aether_oneliner_array_append.out 2>&1
printf 'count = 4\n' >/tmp/aether_oneliner_array_append_expected.out
if ! cmp -s /tmp/aether_oneliner_array_append_expected.out /tmp/aether_oneliner_array_append.out; then
    echo "unexpected one-liner array-append output" >&2
    cat /tmp/aether_oneliner_array_append.out >&2
    exit 1
fi

# Real socket* API end to end: same-process TCP client+server pair
# coordinated with par (listener created/bound/listening before the par
# block starts, so the client branch can't race a not-yet-listening server).
# socketaccept/socketreceive block the calling task until a peer shows up, so
# this runs in the background with a hard kill after 20s instead of a bare
# foreground call — a regression here should fail loudly, not hang the suite.
"$AETHER_BIN" --no-cache "$SOCKET_ECHO_PASS_FIXTURE" >/tmp/aether_socket_echo_pass.out 2>&1 &
socket_echo_pid=$!
socket_echo_waited=0
while kill -0 "$socket_echo_pid" 2>/dev/null; do
    sleep 1
    socket_echo_waited=$((socket_echo_waited + 1))
    if [ "$socket_echo_waited" -ge 20 ]; then
        kill -9 "$socket_echo_pid" 2>/dev/null
        echo "socket echo fixture hung past 20s (accept/receive blocked with no peer?)" >&2
        exit 1
    fi
done
if ! wait "$socket_echo_pid"; then
    echo "socket echo fixture exited non-zero" >&2
    cat /tmp/aether_socket_echo_pass.out >&2
    exit 1
fi
printf 'server got: ping\nclient got: pong\ndone\n' >/tmp/aether_socket_echo_expected.out
if ! cmp -s /tmp/aether_socket_echo_expected.out /tmp/aether_socket_echo_pass.out; then
    echo "unexpected socket echo output" >&2
    cat /tmp/aether_socket_echo_pass.out >&2
    exit 1
fi

# File type end to end: assign/rewrite/writeln/close then reset/readln/eof/
# close then erase, using Aether's own `File` type (lowers to rea's `text`
# keyword / TYPE_FILE). Regression for "no file-content read/write API
# reachable from Aether's own type system" (docs/ideas_and_todo.md).
"$AETHER_BIN" --no-cache "$TESTS_DIR/file_io_pass.aether" >/tmp/aether_file_io_pass.out
printf 'read 1: alpha\nread 2: beta\nexists_after_erase=false\n' >/tmp/aether_file_io_pass_expected.out
if ! cmp -s /tmp/aether_file_io_pass_expected.out /tmp/aether_file_io_pass.out; then
    echo "unexpected File type output (assign/rewrite/writeln/reset/readln/eof/close/erase)" >&2
    cat /tmp/aether_file_io_pass.out >&2
    exit 1
fi

# Legacy-method-call check must look before leaping: docs/ideas_and_todo.md
# "A top-level function named `<TypeName>_word` is unconditionally rejected,
# even when correctly declared". `db_open` is a real, unambiguous top-level
# fn -- the check used to fire on the underscore split alone (`db` matches
# `DB` case-insensitively) ahead of normal call resolution.
"$AETHER_BIN" --no-cache "$LEGACY_METHOD_CALL_SHADOW_PASS_FIXTURE" >/tmp/aether_legacy_method_call_shadow_pass.out
printf '42\n' >/tmp/aether_legacy_method_call_shadow_expected.out
if ! cmp -s /tmp/aether_legacy_method_call_shadow_expected.out /tmp/aether_legacy_method_call_shadow_pass.out; then
    echo "unexpected legacy-method-call shadow output (regression: TypeName_word fn rejected despite valid declaration)" >&2
    cat /tmp/aether_legacy_method_call_shadow_pass.out >&2
    exit 1
fi
# The flip side: an underscore-prefixed call in the same shape but with no
# matching declaration anywhere must still fail to compile -- fixing the
# check to look before leaping must not silently make genuine
# undefined-identifier errors disappear.
if "$AETHER_BIN" --no-cache "$LEGACY_METHOD_CALL_UNDEFINED_FAIL_FIXTURE" >/tmp/aether_legacy_method_call_undefined_fail.out 2>&1; then
    echo "expected legacy-method-call undefined failure but program succeeded" >&2
    cat /tmp/aether_legacy_method_call_undefined_fail.out >&2
    exit 1
fi
if ! grep -q "identifier 'db_missing' not in scope" /tmp/aether_legacy_method_call_undefined_fail.out; then
    echo "missing legacy-method-call undefined-identifier diagnostic" >&2
    cat /tmp/aether_legacy_method_call_undefined_fail.out >&2
    exit 1
fi

echo "aether smoke tests passed"
