#include <stdio.h>

#include "./basm.h"

Binding *basm_resolve_binding(Basm *basm, String_View name)
{
    for (size_t i = 0; i < basm->bindings_size; ++i) {
        if (sv_eq(basm->bindings[i].name, name)) {
            return &basm->bindings[i];
        }
    }

    return NULL;
}

void basm_bind_value(Basm *basm, String_View name, Word value, Binding_Kind kind, File_Location location)
{
    assert(basm->bindings_size < BASM_BINDINGS_CAPACITY);

    Binding *existing = basm_resolve_binding(basm, name);
    if (existing) {
        fprintf(stderr,
                FL_Fmt": ERROR: name `"SV_Fmt"` is already bound\n",
                FL_Arg(location),
                SV_Arg(name));
        fprintf(stderr,
                FL_Fmt": NOTE: first binding is located here\n",
                FL_Arg(existing->location));
        exit(1);
    }

    basm->bindings[basm->bindings_size++] = (Binding) {
        .name = name,
        .value = value,
        .status = BINDING_EVALUATED,
        .kind = kind,
        .location = location,
    };
}

void basm_bind_expr(Basm *basm, String_View name, Expr expr, Binding_Kind kind, File_Location location)
{
    assert(basm->bindings_size < BASM_BINDINGS_CAPACITY);

    Binding *existing = basm_resolve_binding(basm, name);
    if (existing) {
        fprintf(stderr,
                FL_Fmt": ERROR: name `"SV_Fmt"` is already bound\n",
                FL_Arg(location),
                SV_Arg(name));
        fprintf(stderr,
                FL_Fmt": NOTE: first binding is located here\n",
                FL_Arg(existing->location));
        exit(1);
    }

    basm->bindings[basm->bindings_size++] = (Binding) {
        .name = name,
        .expr = expr,
        .kind = kind,
        .location = location,
    };
}

void basm_push_deferred_operand(Basm *basm, Inst_Addr addr, Expr expr, File_Location location)
{
    assert(basm->deferred_operands_size < BASM_DEFERRED_OPERANDS_CAPACITY);
    basm->deferred_operands[basm->deferred_operands_size++] =
    (Deferred_Operand) {
        .addr = addr, .expr = expr, .location = location
    };
}

Word basm_push_string_to_memory(Basm *basm, String_View sv)
{
    assert(basm->memory_size + sv.count <= BM_MEMORY_CAPACITY);

    Word result = word_u64(basm->memory_size);
    memcpy(basm->memory + basm->memory_size, sv.data, sv.count);
    basm->memory_size += sv.count;

    if (basm->memory_size > basm->memory_capacity) {
        basm->memory_capacity = basm->memory_size;
    }

    basm->string_lengths[basm->string_lengths_size++] = (String_Length) {
        .addr = result.as_u64,
        .length = sv.count,
    };

    return result;
}

bool basm_string_length_by_addr(Basm *basm, Inst_Addr addr, Word *length)
{
    for (size_t i = 0; i < basm->string_lengths_size; ++i) {
        if (basm->string_lengths[i].addr == addr) {
            if (length) {
                *length = word_u64(basm->string_lengths[i].length);
            }
            return true;
        }
    }

    return false;
}

const char *binding_kind_as_cstr(Binding_Kind kind)
{
    switch (kind) {
    case BINDING_CONST:
        return "const";
    case BINDING_LABEL:
        return "label";
    case BINDING_NATIVE:
        return "native";
    default:
        assert(false && "binding_kind_as_cstr: unreachable");
        exit(0);
    }
}

void basm_save_to_file(Basm *basm, const char *file_path)
{
    FILE *f = fopen(file_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "ERROR: Could not open file `%s`: %s\n",
                file_path, strerror(errno));
        exit(1);
    }

    Bm_File_Meta meta = {
        .magic = BM_FILE_MAGIC,
        .version = BM_FILE_VERSION,
        .entry = basm->entry,
        .program_size = basm->program_size,
        .memory_size = basm->memory_size,
        .memory_capacity = basm->memory_capacity,
    };

    fwrite(&meta, sizeof(meta), 1, f);
    if (ferror(f)) {
        fprintf(stderr, "ERROR: Could not write to file `%s`: %s\n",
                file_path, strerror(errno));
        exit(1);
    }

    fwrite(basm->program, sizeof(basm->program[0]), basm->program_size, f);
    if (ferror(f)) {
        fprintf(stderr, "ERROR: Could not write to file `%s`: %s\n",
                file_path, strerror(errno));
        exit(1);
    }

    fwrite(basm->memory, sizeof(basm->memory[0]), basm->memory_size, f);
    if (ferror(f)) {
        fprintf(stderr, "ERROR: Could not write to file `%s`: %s\n",
                file_path, strerror(errno));
        exit(1);
    }

    fclose(f);
}

static void basm_translate_bind_directive(Basm *basm, String_View *line, File_Location location, Binding_Kind binding_kind)
{
    *line = sv_trim(*line);
    String_View name = sv_chop_by_delim(line, ' ');
    if (name.count > 0) {
        *line = sv_trim(*line);
        Expr expr = parse_expr_from_sv(&basm->arena, *line, location);

        basm_bind_expr(basm, name, expr, binding_kind, location);

    } else {
        fprintf(stderr,
                FL_Fmt": ERROR: binding name is not provided\n",
                FL_Arg(location));
        exit(1);
    }
}

void basm_translate_source(Basm *basm, String_View input_file_path)
{
    String_View original_source = {0};
    if (arena_slurp_file(&basm->arena, input_file_path, &original_source) < 0) {
        if (basm->include_level > 0) {
            fprintf(stderr, FL_Fmt": ERROR: could not read file `"SV_Fmt"`: %s\n",
                    FL_Arg(basm->include_location),
                    SV_Arg(input_file_path), strerror(errno));
        } else {
            fprintf(stderr, "ERROR: could not read file `"SV_Fmt"`: %s\n",
                    SV_Arg(input_file_path), strerror(errno));
        }
        exit(1);
    }

    String_View source = original_source;

    File_Location location = {
        .file_path = input_file_path,
    };

    // First pass
    while (source.count > 0) {
        String_View line = sv_trim(sv_chop_by_delim(&source, '\n'));
        line = sv_trim(sv_chop_by_delim(&line, BASM_COMMENT_SYMBOL));
        location.line_number += 1;
        if (line.count > 0) {
            String_View token = sv_trim(sv_chop_by_delim(&line, ' '));

            // Pre-processor
            if (token.count > 0 && *token.data == BASM_PP_SYMBOL) {
                token.count -= 1;
                token.data  += 1;
                if (sv_eq(token, sv_from_cstr("bind"))) {
                    fprintf(stderr, FL_Fmt": ERROR: %%bind directive has been removed! Use %%const directive to define consts. Use %%native directive to define native functions.\n", FL_Arg(location));
                    exit(1);
                } else if (sv_eq(token, sv_from_cstr("const"))) {
                    basm_translate_bind_directive(basm, &line, location, BINDING_CONST);
                } else if (sv_eq(token, sv_from_cstr("native"))) {
                    basm_translate_bind_directive(basm, &line, location, BINDING_NATIVE);
                } else if (sv_eq(token, sv_from_cstr("assert"))) {
                    Expr expr = parse_expr_from_sv(&basm->arena, sv_trim(line), location);
                    basm->deferred_asserts[basm->deferred_asserts_size++] = (Deferred_Assert) {
                        .expr = expr,
                        .location = location,
                    };
                } else if (sv_eq(token, sv_from_cstr("include"))) {
                    line = sv_trim(line);


                    if (line.count > 0) {
                        if (*line.data == '"' && line.data[line.count - 1] == '"') {
                            line.data  += 1;
                            line.count -= 2;

                            if (basm->include_level + 1 >= BASM_MAX_INCLUDE_LEVEL) {
                                fprintf(stderr,
                                        FL_Fmt": ERROR: exceeded maximum include level\n",
                                        FL_Arg(location));
                                exit(1);
                            }

                            {
                                File_Location prev_include_location = basm->include_location;
                                basm->include_level += 1;
                                basm->include_location = location;
                                basm_translate_source(basm, line);
                                basm->include_location = prev_include_location;
                                basm->include_level -= 1;
                            }
                        } else {
                            fprintf(stderr,
                                    FL_Fmt": ERROR: include file path has to be surrounded with quotation marks\n",
                                    FL_Arg(location));
                            exit(1);
                        }
                    } else {
                        fprintf(stderr,
                                FL_Fmt": ERROR: include file path is not provided\n",
                                FL_Arg(location));
                        exit(1);
                    }
                } else if (sv_eq(token, sv_from_cstr("entry"))) {
                    if (basm->has_entry) {
                        fprintf(stderr,
                                FL_Fmt": ERROR: entry point has been already set!\n",
                                FL_Arg(location));
                        fprintf(stderr, FL_Fmt": NOTE: the first entry point\n", FL_Arg(basm->entry_location));
                        exit(1);
                    }

                    line = sv_trim(line);

                    Expr expr = parse_expr_from_sv(&basm->arena, line, location);

                    if (expr.kind != EXPR_KIND_BINDING) {
                        fprintf(stderr, FL_Fmt": ERROR: only bindings are allowed to be set as entry points for now.\n",
                                FL_Arg(location));
                        exit(1);
                    }

                    basm->deferred_entry_binding_name = expr.value.as_binding;
                    basm->has_entry = true;
                    basm->entry_location = location;
                } else {
                    fprintf(stderr,
                            FL_Fmt": ERROR: unknown pre-processor directive `"SV_Fmt"`\n",
                            FL_Arg(location),
                            SV_Arg(token));
                    exit(1);

                }
            } else {
                // Label binding
                if (token.count > 0 && token.data[token.count - 1] == ':') {
                    String_View label = {
                        .count = token.count - 1,
                        .data = token.data
                    };

                    basm_bind_value(basm, label, word_u64(basm->program_size), BINDING_LABEL, location);
                    token = sv_trim(sv_chop_by_delim(&line, ' '));
                }

                // Instruction
                if (token.count > 0) {
                    String_View operand = line;
                    Inst_Type inst_type = INST_NOP;
                    if (inst_by_name(token, &inst_type)) {
                        assert(basm->program_size < BM_PROGRAM_CAPACITY);
                        basm->program[basm->program_size].type = inst_type;

                        if (inst_has_operand(inst_type)) {
                            if (operand.count == 0) {
                                fprintf(stderr, FL_Fmt": ERROR: instruction `"SV_Fmt"` requires an operand\n",
                                        FL_Arg(location),
                                        SV_Arg(token));
                                exit(1);
                            }

                            Expr expr = parse_expr_from_sv(&basm->arena, operand, location);

                            if (expr.kind == EXPR_KIND_BINDING) {
                                basm_push_deferred_operand(basm, basm->program_size, expr, location);
                            } else {
                                assert(expr.kind != EXPR_KIND_BINDING);
                                basm->program[basm->program_size].operand =
                                    basm_expr_eval(basm, expr, location);
                            }
                        }

                        basm->program_size += 1;
                    } else {
                        fprintf(stderr, FL_Fmt": ERROR: unknown instruction `"SV_Fmt"`\n",
                                FL_Arg(location),
                                SV_Arg(token));
                        exit(1);
                    }
                }
            }
        }
    }

    // Second pass
    for (size_t i = 0; i < basm->deferred_operands_size; ++i) {
        Expr expr = basm->deferred_operands[i].expr;
        assert(expr.kind == EXPR_KIND_BINDING);
        String_View name = expr.value.as_binding;

        Inst_Addr addr = basm->deferred_operands[i].addr;
        Binding *binding = basm_resolve_binding(basm, name);
        if (binding == NULL) {
            fprintf(stderr, FL_Fmt": ERROR: unknown binding `"SV_Fmt"`\n",
                    FL_Arg(basm->deferred_operands[i].location),
                    SV_Arg(name));
            exit(1);
        }

        if (basm->program[addr].type == INST_CALL && binding->kind != BINDING_LABEL) {
            fprintf(stderr, FL_Fmt": ERROR: trying to call not a label. `"SV_Fmt"` is %s, but the call instructions accepts only literals or labels.\n", FL_Arg(basm->deferred_operands[i].location), SV_Arg(name), binding_kind_as_cstr(binding->kind));
            exit(1);
        }

        if (basm->program[addr].type == INST_NATIVE && binding->kind != BINDING_NATIVE) {
            fprintf(stderr, FL_Fmt": ERROR: trying to invoke native function from a binding that is %s. Bindings for native functions have to be defined via `%%native` basm directive.\n", FL_Arg(basm->deferred_operands[i].location), binding_kind_as_cstr(binding->kind));
            exit(1);
        }

        basm->program[addr].operand = basm_binding_eval(basm, binding, basm->deferred_operands[i].location);
    }

    // Eval deferred asserts
    for (size_t i = 0; i < basm->deferred_asserts_size; ++i) {
        Word value = basm_expr_eval(
                         basm,
                         basm->deferred_asserts[i].expr,
                         basm->deferred_asserts[i].location);
        if (!value.as_u64) {
            fprintf(stderr, FL_Fmt": ERROR: assertion failed\n",
                    FL_Arg(basm->deferred_asserts[i].location));
            exit(1);
        }
    }

    // Resolving deferred entry point
    if (basm->has_entry && basm->deferred_entry_binding_name.count > 0) {
        Binding *binding = basm_resolve_binding(
                               basm,
                               basm->deferred_entry_binding_name);
        if (binding == NULL) {
            fprintf(stderr, FL_Fmt": ERROR: unknown binding `"SV_Fmt"`\n",
                    FL_Arg(basm->entry_location),
                    SV_Arg(basm->deferred_entry_binding_name));
            exit(1);
        }

        if (binding->kind != BINDING_LABEL) {
            fprintf(stderr, FL_Fmt": ERROR: trying to set a %s as an entry point. Entry point has to be a label.\n", FL_Arg(basm->entry_location), binding_kind_as_cstr(binding->kind));
            exit(1);
        }

        basm->entry = basm_binding_eval(basm, binding, basm->entry_location).as_u64;
    }
}

Word basm_binding_eval(Basm *basm, Binding *binding, File_Location location)
{
    if (binding->status == BINDING_EVALUATING) {
        fprintf(stderr, FL_Fmt": ERROR: cycling binding definition.\n",
                FL_Arg(binding->location));
        exit(1);
    }

    if (binding->status == BINDING_UNEVALUATED) {
        binding->status = BINDING_EVALUATING;
        Word value = basm_expr_eval(basm, binding->expr, location);
        binding->status = BINDING_EVALUATED;
        binding->value = value;
    }

    return binding->value;
}

static Word basm_binary_op_eval(Basm *basm, Binary_Op *binary_op, File_Location location)
{
    Word left = basm_expr_eval(basm, binary_op->left, location);
    Word right = basm_expr_eval(basm, binary_op->right, location);

    switch (binary_op->kind) {
    case BINARY_OP_PLUS: {
        // TODO(#183): compile-time sum can only work with integers
        return word_u64(left.as_u64 + right.as_u64);
    }
    break;

    case BINARY_OP_GT: {
        return word_u64(left.as_u64 > right.as_u64);
    }
    break;

    default: {
        assert(false && "basm_binary_op_eval: unreachable");
        exit(1);
    }
    }
}

Word basm_expr_eval(Basm *basm, Expr expr, File_Location location)
{
    switch (expr.kind) {
    case EXPR_KIND_LIT_INT:
        return word_u64(expr.value.as_lit_int);

    case EXPR_KIND_LIT_FLOAT:
        return word_f64(expr.value.as_lit_float);

    case EXPR_KIND_LIT_CHAR:
        return word_u64((uint64_t) expr.value.as_lit_char);

    case EXPR_KIND_LIT_STR:
        return basm_push_string_to_memory(basm, expr.value.as_lit_str);

    case EXPR_KIND_FUNCALL: {
        if (sv_eq(expr.value.as_funcall->name, sv_from_cstr("len"))) {
            const size_t actual_arity = funcall_args_len(expr.value.as_funcall->args);
            if (actual_arity != 1) {
                fprintf(stderr, FL_Fmt": ERROR: len() expects 1 argument but got %zu\n",
                        FL_Arg(location), actual_arity);
                exit(1);
            }

            Word addr = basm_expr_eval(basm, expr.value.as_funcall->args->value, location);
            Word length = {0};
            if (!basm_string_length_by_addr(basm, addr.as_u64, &length)) {
                fprintf(stderr, FL_Fmt": ERROR: Could not compute the length of string at address %"PRIu64"\n", FL_Arg(location), addr.as_u64);
                exit(1);
            }

            return length;
        } else {
            fprintf(stderr,
                    FL_Fmt": ERROR: Unknown translation time function `"SV_Fmt"`\n",
                    FL_Arg(location), SV_Arg(expr.value.as_funcall->name));
            exit(1);
        }
    }
    break;

    case EXPR_KIND_BINDING: {
        String_View name = expr.value.as_binding;
        Binding *binding = basm_resolve_binding(basm, name);
        if (binding == NULL) {
            fprintf(stderr, FL_Fmt": ERROR: could find binding `"SV_Fmt"`.\n",
                    FL_Arg(location), SV_Arg(name));
            exit(1);
        }

        return basm_binding_eval(basm, binding, location);
    }
    break;

    case EXPR_KIND_BINARY_OP: {
        return basm_binary_op_eval(basm, expr.value.as_binary_op, location);
    }
    break;

    default: {
        assert(false && "basm_expr_eval: unreachable");
        exit(1);
    }
    break;
    }
}

const char *token_kind_name(Token_Kind kind)
{
    switch (kind) {
    case TOKEN_KIND_STR:
        return "string";
    case TOKEN_KIND_CHAR:
        return "character";
    case TOKEN_KIND_PLUS:
        return "plus";
    case TOKEN_KIND_MINUS:
        return "minus";
    case TOKEN_KIND_NUMBER:
        return "number";
    case TOKEN_KIND_NAME:
        return "name";
    case TOKEN_KIND_OPEN_PAREN:
        return "open paren";
    case TOKEN_KIND_CLOSING_PAREN:
        return "closing paren";
    case TOKEN_KIND_COMMA:
        return "comma";
    case TOKEN_KIND_GT:
        return ">";
    default: {
        assert(false && "token_kind_name: unreachable");
        exit(1);
    }
    }
}

void tokens_push(Tokens *tokens, Token token)
{
    assert(tokens->count < TOKENS_CAPACITY);
    tokens->elems[tokens->count++] = token;
}

static bool is_name(char x)
{
    return isalnum(x) || x == '_';
}

static bool is_number(char x)
{
    return isalnum(x) || x == '.';
}

String_View sv_chop_left_while(String_View *sv, bool (*predicate)(char x))
{
    size_t i = 0;
    while (i < sv->count && predicate(sv->data[i])) {
        i += 1;
    }
    return sv_chop_left(sv, i);
}

void tokenize(String_View source, Tokens *tokens, File_Location location)
{
    source = sv_trim_left(source);
    while (source.count > 0) {
        switch (*source.data) {
        case '(': {
            tokens_push(tokens, (Token) {
                .kind = TOKEN_KIND_OPEN_PAREN,
                .text = sv_chop_left(&source, 1)
            });
        }
        break;

        case ')': {
            tokens_push(tokens, (Token) {
                .kind = TOKEN_KIND_CLOSING_PAREN,
                .text = sv_chop_left(&source, 1)
            });
        }
        break;

        case ',': {
            tokens_push(tokens, (Token) {
                .kind = TOKEN_KIND_COMMA,
                .text = sv_chop_left(&source, 1)
            });
        }
        break;

        case '>': {
            tokens_push(tokens, (Token) {
                .kind = TOKEN_KIND_GT,
                .text = sv_chop_left(&source, 1)
            });
        }
        break;

        case '+': {
            tokens_push(tokens, (Token) {
                .kind = TOKEN_KIND_PLUS,
                .text = sv_chop_left(&source, 1)
            });
        }
        break;

        case '-': {
            tokens_push(tokens, (Token) {
                .kind = TOKEN_KIND_MINUS,
                .text = sv_chop_left(&source, 1)
            });
        }
        break;

        case '"': {
            sv_chop_left(&source, 1);

            size_t index = 0;

            if (sv_index_of(source, '"', &index)) {
                String_View text = sv_chop_left(&source, index);
                sv_chop_left(&source, 1);
                tokens_push(tokens, (Token) {
                    .kind = TOKEN_KIND_STR, .text = text
                });
            } else {
                fprintf(stderr, FL_Fmt": ERROR: Could not find closing \"\n",
                        FL_Arg(location));
                exit(1);
            }
        }
        break;

        case '\'': {
            sv_chop_left(&source, 1);

            size_t index = 0;

            if (sv_index_of(source, '\'', &index)) {
                String_View text = sv_chop_left(&source, index);
                sv_chop_left(&source, 1);
                tokens_push(tokens, (Token) {
                    .kind = TOKEN_KIND_CHAR, .text = text
                });
            } else {
                fprintf(stderr, FL_Fmt": ERROR: Could not find closing \'\n",
                        FL_Arg(location));
                exit(1);
            }
        }
        break;

        default: {
            if (isalpha(*source.data)) {
                tokens_push(tokens, (Token) {
                    .kind = TOKEN_KIND_NAME,
                    .text = sv_chop_left_while(&source, is_name)
                });
            } else if (isdigit(*source.data)) {
                tokens_push(tokens, (Token) {
                    .kind = TOKEN_KIND_NUMBER,
                    .text = sv_chop_left_while(&source, is_number)
                });
            } else {
                fprintf(stderr, FL_Fmt": ERROR: Unknown token starts with %c\n",
                        FL_Arg(location), *source.data);
                exit(1);
            }
        }
        }

        source = sv_trim_left(source);
    }
}

Tokens_View tokens_as_view(const Tokens *tokens)
{
    return (Tokens_View) {
        .elems = tokens->elems,
        .count = tokens->count,
    };
}

Tokens_View tv_chop_left(Tokens_View *tv, size_t n)
{
    if (n > tv->count) {
        n = tv->count;
    }

    Tokens_View result = {
        .elems = tv->elems,
        .count = n,
    };

    tv->elems += n;
    tv->count -= n;

    return result;
}

static Expr parse_number_from_tokens(Arena *arena, Tokens_View *tokens, File_Location location)
{
    if (tokens->count == 0) {
        fprintf(stderr, FL_Fmt": ERROR: Cannot parse empty expression\n",
                FL_Arg(location));
        exit(1);
    }

    Expr result = {0};

    if (tokens->elems->kind == TOKEN_KIND_NUMBER) {
        String_View text = tokens->elems->text;

        const char *cstr = arena_sv_to_cstr(arena, text);
        char *endptr = 0;

        if (sv_has_prefix(text, sv_from_cstr("0x"))) {
            result.value.as_lit_int = strtoull(cstr, &endptr, 16);
            if ((size_t) (endptr - cstr) != text.count) {
                fprintf(stderr, FL_Fmt": ERROR: `"SV_Fmt"` is not a hex literal\n",
                        FL_Arg(location), SV_Arg(text));
                exit(1);
            }

            result.kind = EXPR_KIND_LIT_INT;
            tv_chop_left(tokens, 1);
        } else {
            result.value.as_lit_int = strtoull(cstr, &endptr, 10);
            if ((size_t) (endptr - cstr) != text.count) {
                result.value.as_lit_float = strtod(cstr, &endptr);
                if ((size_t) (endptr - cstr) != text.count) {
                    fprintf(stderr, FL_Fmt": ERROR: `"SV_Fmt"` is not a number literal\n",
                            FL_Arg(location), SV_Arg(text));
                } else {
                    result.kind = EXPR_KIND_LIT_FLOAT;
                }
            } else {
                result.kind = EXPR_KIND_LIT_INT;
            }

            tv_chop_left(tokens, 1);
        }
    } else {
        fprintf(stderr, FL_Fmt": ERROR: expected %s but got %s",
                FL_Arg(location),
                token_kind_name(TOKEN_KIND_NUMBER),
                token_kind_name(tokens->elems->kind));
        exit(1);
    }

    return result;
}

// TODO(#199): parse_primary_from_tokens does not support parens
Expr parse_primary_from_tokens(Arena *arena, Tokens_View *tokens, File_Location location)
{
    if (tokens->count == 0) {
        fprintf(stderr, FL_Fmt": ERROR: Cannot parse empty expression\n",
                FL_Arg(location));
        exit(1);
    }

    Expr result = {0};

    switch (tokens->elems->kind) {
    case TOKEN_KIND_STR: {
        // TODO(#66): string literals don't support escaped characters
        result.kind = EXPR_KIND_LIT_STR;
        result.value.as_lit_str = tokens->elems->text;
        tv_chop_left(tokens, 1);
    }
    break;

    case TOKEN_KIND_CHAR: {
        if (tokens->elems->text.count != 1) {
            // TODO(#179): char literals don't support escaped characters
            fprintf(stderr, FL_Fmt": ERROR: the length of char literal has to be exactly one\n",
                    FL_Arg(location));
            exit(1);
        }

        result.kind = EXPR_KIND_LIT_CHAR;
        result.value.as_lit_char = tokens->elems->text.data[0];
        tv_chop_left(tokens, 1);
    }
    break;

    case TOKEN_KIND_NAME: {
        if (tokens->count > 1 && tokens->elems[1].kind == TOKEN_KIND_OPEN_PAREN) {
            result.kind = EXPR_KIND_FUNCALL;
            result.value.as_funcall = arena_alloc(arena, sizeof(Funcall));
            result.value.as_funcall->name = tokens->elems->text;
            tv_chop_left(tokens, 1);
            result.value.as_funcall->args = parse_funcall_args(arena, tokens, location);
        } else {
            result.value.as_binding = tokens->elems->text;
            result.kind = EXPR_KIND_BINDING;
            tv_chop_left(tokens, 1);
        }
    }
    break;

    case TOKEN_KIND_NUMBER: {
        return parse_number_from_tokens(arena, tokens, location);
    }
    break;

    case TOKEN_KIND_MINUS: {
        tv_chop_left(tokens, 1);
        Expr expr = parse_number_from_tokens(arena, tokens, location);

        if (expr.kind == EXPR_KIND_LIT_INT) {
            // TODO(#184): more cross-platform way to negate integer literals
            // what if somewhere the numbers are not two's complement
            expr.value.as_lit_int = (~expr.value.as_lit_int + 1);
        } else if (expr.kind == EXPR_KIND_LIT_FLOAT) {
            expr.value.as_lit_float = -expr.value.as_lit_float;
        } else {
            assert(false && "unreachable");
        }

        return expr;
    }
    break;

    case TOKEN_KIND_GT:
    case TOKEN_KIND_OPEN_PAREN:
    case TOKEN_KIND_COMMA:
    case TOKEN_KIND_CLOSING_PAREN:
    case TOKEN_KIND_PLUS: {
        fprintf(stderr, FL_Fmt": ERROR: expected primary expression but found %s\n",
                FL_Arg(location), token_kind_name(tokens->elems->kind));
        exit(1);
    }
    break;

    default: {
        assert(false && "parse_primary_from_tokens: unreachable");
        exit(1);
    }
    }

    return result;
}

size_t funcall_args_len(Funcall_Arg *args)
{
    size_t result = 0;
    while (args != NULL) {
        result += 1;
        args = args->next;
    }
    return result;
}

void dump_expr(FILE *stream, Expr expr, int level)
{
    fprintf(stream, "%*s", level * 2, "");

    switch(expr.kind) {
    case EXPR_KIND_BINDING:
        fprintf(stream, "Binding: "SV_Fmt"\n",
                SV_Arg(expr.value.as_binding));
        break;
    case EXPR_KIND_LIT_INT:
        fprintf(stream, "Int Literal: %"PRIu64"\n", expr.value.as_lit_int);
        break;
    case EXPR_KIND_LIT_FLOAT:
        fprintf(stream, "Float Literal: %lf\n", expr.value.as_lit_float);
        break;
    case EXPR_KIND_LIT_CHAR:
        fprintf(stream, "Char Literal: '%c'\n", expr.value.as_lit_char);
        break;
    case EXPR_KIND_LIT_STR:
        fprintf(stream, "String Literal: \""SV_Fmt"\"\n",
                SV_Arg(expr.value.as_lit_str));
        break;
    case EXPR_KIND_BINARY_OP:
        fprintf(stream, "Binary Op: %s\n",
                binary_op_kind_name(expr.value.as_binary_op->kind));
        dump_binary_op(stream, *expr.value.as_binary_op, level + 1);
        break;
    case EXPR_KIND_FUNCALL:
        fprintf(stream, "Funcall: "SV_Fmt"\n",
                SV_Arg(expr.value.as_funcall->name));
        dump_funcall_args(stream, expr.value.as_funcall->args, level + 1);
        break;
    }
}

static int dump_expr_as_dot_edges(FILE *stream, Expr expr, int *counter)
{
    int id = (*counter)++;

    switch (expr.kind) {
    case EXPR_KIND_BINDING: {
        fprintf(stream, "Expr_%d [shape=box label=\""SV_Fmt"\"]\n",
                id, SV_Arg(expr.value.as_binding));
    }
    break;
    case EXPR_KIND_LIT_INT: {
        fprintf(stream, "Expr_%d [shape=circle label=\"%"PRIu64"\"]\n",
                id, expr.value.as_lit_int);
    }
    break;
    case EXPR_KIND_LIT_FLOAT: {
        fprintf(stream, "Expr_%d [shape=circle label=\"%lf\"]\n",
                id, expr.value.as_lit_float);
    }
    break;
    case EXPR_KIND_LIT_CHAR: {
        fprintf(stream, "Expr_%d [shape=circle label=\"'%c'\"]\n",
                id, expr.value.as_lit_char);
    }
    break;
    case EXPR_KIND_LIT_STR: {
        fprintf(stream, "Expr_%d [shape=circle label=\""SV_Fmt"\"]\n",
                id, SV_Arg(expr.value.as_lit_str));
    }
    break;
    case EXPR_KIND_BINARY_OP: {
        fprintf(stream, "Expr_%d [shape=diamond label=\"%s\"]\n",
                id, binary_op_kind_name(expr.value.as_binary_op->kind));
        int left_id = dump_expr_as_dot_edges(stream, expr.value.as_binary_op->left, counter);
        int right_id = dump_expr_as_dot_edges(stream, expr.value.as_binary_op->right, counter);
        fprintf(stream, "Expr_%d -> Expr_%d\n", id, left_id);
        fprintf(stream, "Expr_%d -> Expr_%d\n", id, right_id);
    }
    break;
    case EXPR_KIND_FUNCALL: {
        fprintf(stream, "Expr_%d [shape=diamond label=\""SV_Fmt"\"]\n",
                id, SV_Arg(expr.value.as_funcall->name));

        for (Funcall_Arg *arg = expr.value.as_funcall->args;
                arg != NULL;
                arg = arg->next) {
            int child_id = dump_expr_as_dot_edges(stream, arg->value, counter);
            fprintf(stream, "Expr_%d -> Expr_%d\n", id, child_id);
        }
    }
    break;
    }

    return id;
}

void dump_expr_as_dot(FILE *stream, Expr expr)
{
    fprintf(stream, "digraph Expr {\n");
    int counter = 0;
    dump_expr_as_dot_edges(stream, expr, &counter);
    fprintf(stream, "}\n");
}

const char *binary_op_kind_name(Binary_Op_Kind kind)
{
    switch (kind) {
    case BINARY_OP_PLUS:
        return "+";
    case BINARY_OP_GT:
        return ">";
    default:
        assert(false && "binary_op_kind_name: unreachable");
        exit(1);
    }
}

void dump_binary_op(FILE *stream, Binary_Op binary_op, int level)
{
    fprintf(stream, "%*sLeft:\n", level * 2, "");
    dump_expr(stream, binary_op.left, level + 1);

    fprintf(stream, "%*sRight:\n", level * 2, "");
    dump_expr(stream, binary_op.right, level + 1);
}

void dump_funcall_args(FILE *stream, Funcall_Arg *args, int level)
{
    while (args != NULL) {
        fprintf(stream, "%*sArg:\n", level * 2, "");
        dump_expr(stream, args->value, level + 1);
        args = args->next;
    }
}

Funcall_Arg *parse_funcall_args(Arena *arena, Tokens_View *tokens, File_Location location)
{
    if (tokens->count < 1 || tokens->elems->kind != TOKEN_KIND_OPEN_PAREN) {
        fprintf(stderr, FL_Fmt": ERROR: expected %s\n",
                FL_Arg(location),
                token_kind_name(TOKEN_KIND_OPEN_PAREN));
        exit(1);
    }
    tv_chop_left(tokens, 1);

    if (tokens->count > 0 && tokens->elems->kind == TOKEN_KIND_CLOSING_PAREN) {
        return NULL;
    }

    Funcall_Arg *first = NULL;
    Funcall_Arg *last = NULL;

    // , b, c)
    Token token = {0};
    do {
        Funcall_Arg *arg = arena_alloc(arena, sizeof(Funcall_Arg));
        arg->value = parse_expr_from_tokens(arena, tokens, location);

        if (first == NULL) {
            first = arg;
            last = arg;
        } else {
            last->next = arg;
            last = arg;
        }

        if (tokens->count == 0) {
            fprintf(stderr, FL_Fmt": ERROR: expected %s or %s\n",
                    FL_Arg(location),
                    token_kind_name(TOKEN_KIND_CLOSING_PAREN),
                    token_kind_name(TOKEN_KIND_COMMA));
            exit(1);
        }

        token = tv_chop_left(tokens, 1).elems[0];
    } while (token.kind == TOKEN_KIND_COMMA);

    if (token.kind != TOKEN_KIND_CLOSING_PAREN) {
        fprintf(stderr, FL_Fmt": ERROR: expected %s\n",
                FL_Arg(location),
                token_kind_name(TOKEN_KIND_CLOSING_PAREN));
        exit(1);
    }

    return first;
}

Expr parse_gt_from_tokens(Arena *arena, Tokens_View *tokens, File_Location location)
{
    Expr left = parse_sum_from_tokens(arena, tokens, location);

    if (tokens->count != 0 && tokens->elems->kind == TOKEN_KIND_GT) {
        tv_chop_left(tokens, 1);

        Expr right = parse_gt_from_tokens(arena, tokens, location);

        Binary_Op *binary_op = arena_alloc(arena, sizeof(Binary_Op));
        binary_op->kind = BINARY_OP_GT;
        binary_op->left = left;
        binary_op->right = right;

        Expr result = {
            .kind = EXPR_KIND_BINARY_OP,
            .value = {
                .as_binary_op = binary_op,
            }
        };

        return result;
    } else {
        return left;
    }
}

Expr parse_sum_from_tokens(Arena *arena, Tokens_View *tokens, File_Location location)
{
    Expr left = parse_primary_from_tokens(arena, tokens, location);

    if (tokens->count != 0 && tokens->elems->kind == TOKEN_KIND_PLUS) {
        tv_chop_left(tokens, 1);

        Expr right = parse_sum_from_tokens(arena, tokens, location);

        Binary_Op *binary_op = arena_alloc(arena, sizeof(Binary_Op));
        binary_op->kind = BINARY_OP_PLUS;
        binary_op->left = left;
        binary_op->right = right;

        Expr result = {
            .kind = EXPR_KIND_BINARY_OP,
            .value = {
                .as_binary_op = binary_op,
            }
        };

        return result;
    } else {
        return left;
    }
}

Expr parse_expr_from_tokens(Arena *arena, Tokens_View *tokens, File_Location location)
{
    return parse_gt_from_tokens(arena, tokens, location);
}

Expr parse_expr_from_sv(Arena *arena, String_View source, File_Location location)
{
    Tokens tokens = {0};
    tokenize(source, &tokens, location);

    Tokens_View tv = tokens_as_view(&tokens);
    return parse_expr_from_tokens(arena, &tv, location);
}
