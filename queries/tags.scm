(value_declaration (function_declaration_left (lower_case_identifier) @name)) @definition.function

(function_call_expr (value_expr (value_qid) @name)) @reference.function
(exposed_value (lower_case_identifier) @name) @reference.function
(type_annotation ((lower_case_identifier) @name) (colon)) @reference.function

(type_declaration ((type_identifier) @name) ) @definition.type

(type_ref (type_qid (type_identifier) @name)) @reference.type
(exposed_type (type_identifier) @name) @reference.type

(type_declaration (union_variant (constructor_identifier) @name)) @definition.union

(value_expr (constructor_qid (constructor_identifier) @name)) @reference.union


(module_declaration
    (module_identifier (module_name_segment)) @name
) @definition.module
