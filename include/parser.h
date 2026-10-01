#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"
#include <setjmp.h>

typedef struct
{
	Tokens *ts;
	size_t p;
	Program *prog;
	jmp_buf error_jmp;
	int recovery_brace_depth;
	int scope_depth;
	Param *pending_params;
	size_t npending_params;
	bool enum_definition;
	Decl *current_function;
} Parser;

void parse_program(Tokens *token_stream, Program *prog);
StructMember *find_struct_member(CType *type, const char *name);
bool eval_const_expr(Expr *expression, long *value);

#endif
