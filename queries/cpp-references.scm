; Usage sites of named symbols in C/C++ sources.
;
; Captures every identifier-like occurrence; lookups filter by name and
; definition sites (from the tags query) are flagged so callers can
; exclude them. Comments, strings and preprocessor directives never
; produce these node types.
(identifier) @usage
(type_identifier) @usage
(field_identifier) @usage
(namespace_identifier) @usage