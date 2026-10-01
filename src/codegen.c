#include "codegen.h"

//Code generation
typedef enum
{
	SY_LOCAL,
	SY_GLOBAL,
	SY_CONST,
	SY_EXTERN_GLOBAL,
	SY_FUNCTION,
	SY_EXTERN_FUNCTION
} SymKind;
typedef struct
{
	char *name;
	CType *type;
	SymKind kind;
	long off;
	Decl *fn;
} Symbol;
typedef struct
{
	char *value;
	char label[32];
} StringLit;
typedef struct
{
	Program *prog;
	Buf out;
	Buf ro;
	Symbol *globals;
	size_t nglobals, capglobals;
	Decl **funcs;
	size_t nfuncs, capfuncs;
	Symbol *locals;
	size_t nlocals, caplocals;
	StringLit *strings;
	size_t nstrings, capstrings;
	Decl *current;
	long frame, vaoff, tempdepth;
	long label;
	bool input_used;
	char retlabel[128];
	char **breaks;
	size_t nbreaks, capbreaks;
	char **continues;
	size_t ncontinues, capcontinues;
} Gen;

long align_up(long value, long array)
{
	return (value + array - 1) / array * array;
}

static void emit(Gen *generator, const char *fmt, ...)
{
	va_list argument_list, copied_argument_list;
	int count;
	va_start(argument_list, fmt);
	va_copy(copied_argument_list, argument_list);
	count = vsnprintf(NULL, 0, fmt, copied_argument_list);
	va_end(copied_argument_list);
	bneed(&generator->out, (size_t)count + 1);
	vsnprintf(generator->out.s + generator->out.n, generator->out.cap - generator->out.n, fmt, argument_list);
	va_end(argument_list);
	generator->out.n += (size_t)count;
	bputs(&generator->out, "\n");
}

static char *new_label(Gen *generator, const char *prefix)
{
	char label_buffer_size[128];
	snprintf(label_buffer_size, sizeof(label_buffer_size), "%s%ld", prefix, ++generator->label);
	return xstrdup(label_buffer_size);
}

static Symbol *find_local(Gen *generator, const char *count)
{
	size_t index;
	for(index = 0; index < generator->nlocals; index++)
		if(!strcmp(generator->locals[index].name, count))
			return &generator->locals[index];
	return NULL;
}

static Symbol *find_global(Gen *generator, const char *count)
{
	size_t index;
	Symbol *external = NULL;
	for(index = 0; index < generator->nglobals; index++) {
		if(strcmp(generator->globals[index].name, count))
			continue;
		if(generator->globals[index].kind == SY_GLOBAL)
			return &generator->globals[index];
		external = &generator->globals[index];
	}
	return external;
}

static Decl *find_func(Gen *generator, const char *count)
{
	size_t index;
	for(index = 0; index < generator->nfuncs; index++)
		if(!strcmp(generator->funcs[index]->name, count))
			return generator->funcs[index];
	return NULL;
}

static Symbol lookup(Gen *generator, const char *count)
{
	Symbol *s = find_local(generator, count);
	Decl *function_declaration;
	if(s)
		return *s;
	s = find_global(generator, count);
	if(s)
		return *s;
	if(!strcmp(count, "NULL"))
		return (Symbol){(char *)count, &T_U64, SY_CONST, 0, NULL};
	if(!strcmp(count, "ExposureMask"))
		return (Symbol){(char *)count, &T_U64, SY_CONST, 1L << 15, NULL};
	if(!strcmp(count, "KeyPressMask"))
		return (Symbol){(char *)count, &T_U64, SY_CONST, 1L << 0, NULL};
	if(!strcmp(count, "KeyPress"))
		return (Symbol){(char *)count, &T_U64, SY_CONST, 2, NULL};
	if(!strcmp(count, "stderr") || !strcmp(count, "stdin"))
		return (Symbol){(char *)count, ptr_to(&T_VOID), SY_EXTERN_GLOBAL, 0, NULL};
	function_declaration = find_func(generator, count);
	if(function_declaration)
		return (Symbol){(char *)count, function_declaration->type, SY_FUNCTION, 0, function_declaration};
	return (Symbol){(char *)count, &T_U64, SY_EXTERN_FUNCTION, 0, NULL};
}

static CType *expr_type(Gen *generator, Expr *expression)
{
	CType *type_1;
	if(expression->type)
		return expression->type;
	switch(expression->kind) {
	case EX_ID:
	{
		Symbol s = lookup(generator, expression->str);
		return s.type;
	}
	case EX_STR:
		return ptr_to(&T_CHAR);
	case EX_ENUM_CONST:
	case EX_NUM:
		return &T_U64;
	case EX_UNARY:
		if(!strcmp(expression->op, "&"))
			return ptr_to(expr_type(generator, expression->left));
		if(!strcmp(expression->op, "*")) {
			type_1 = expr_type(generator, expression->left);
			return type_1->base ? type_1->base : &T_U64;
		}
		return expr_type(generator, expression->left);
	case EX_INDEX:
		type_1 = expr_type(generator, expression->left);
		return type_1->base ? type_1->base : &T_U64;
	case EX_MEMBER:
	case EX_PTRMEMBER:
	{
		StructMember *member;
		type_1 = expr_type(generator, expression->left);
		if(expression->kind == EX_PTRMEMBER)
			type_1 = type_1 && type_1->kind == TY_PTR ? type_1->base : NULL;
		member = find_struct_member(type_1, expression->str);
		if(!member)
			fatal("unknown struct member %s", expression->str);
		return member->type;
	}
	case EX_SIZEOF:
		return &T_U64;
	case EX_TYPEOF:
		return expression->sizeof_type ? expression->sizeof_type : expr_type(generator, expression->left);
	case EX_TYPE:
		return expression->type;
	case EX_BINARY:
		if(!strcmp(expression->op, "==") || !strcmp(expression->op, "!=") || !strcmp(expression->op, "<") || !strcmp(expression->op, "<=") || !strcmp(expression->op, ">") || !strcmp(expression->op, ">=") || !strcmp(expression->op, "&&") || !strcmp(expression->op, "||"))
			return &T_INT;
		if(!strcmp(expression->op, "=") || !strcmp(expression->op, "+=") || !strcmp(expression->op, "-=") ||
		   !strcmp(expression->op, "*=") || !strcmp(expression->op, "/="))
			return expr_type(generator, expression->left);
		if(expr_type(generator, expression->left)->kind == TY_DOUBLE ||
		   expr_type(generator, expression->right)->kind == TY_DOUBLE)
			return &T_DOUBLE;
		return expr_type(generator, expression->left);
	case EX_CALL:
		if(expression->left->kind == EX_ID) {
			if(!strcmp(expression->left->str, "input"))
				return ptr_to(&T_CHAR);
			Decl *function_declaration = find_func(generator, expression->left->str);
			if(function_declaration)
				return function_declaration->type;
		}
		return &T_U64;
	case EX_INITLIST:
		return &T_VOID;
	case EX_MATCH:
	{
		size_t index;
		for(index = 0; index < expression->narms; index++)
			if(expression->arms[index].expr)
				return expr_type(generator, expression->arms[index].expr);
		return &T_INT;
	}
	}
	return &T_U64;
}

static char *intern_string(Gen *generator, const char *value_1)
{
	size_t index, inner_index, length;
	for(index = 0; index < generator->nstrings; index++)
		if(!strcmp(generator->strings[index].value, value_1))
			return generator->strings[index].label;
	ARR_GROW(generator->strings, generator->nstrings, generator->capstrings, StringLit);
	generator->strings[generator->nstrings].value = xstrdup(value_1);
	snprintf(generator->strings[generator->nstrings].label, sizeof(generator->strings[generator->nstrings].label), ".LC%zu", generator->nstrings);
	bprintf(&generator->ro, "%s:\n    .byte ", generator->strings[generator->nstrings].label);
	length = strlen(value_1);
	for(inner_index = 0; inner_index < length; inner_index++)
		bprintf(&generator->ro, "%u, ", (unsigned char)value_1[inner_index]);
	bputs(&generator->ro, "0\n");
	return generator->strings[generator->nstrings++].label;
}
static void gen_expr(Gen *, Expr *);
static CType *gen_addr(Gen *, Expr *);

static void pushreg(Gen *generator, const char *register_name)
{
	emit(generator, "    push %s", register_name);
	generator->tempdepth += 8;
}

static void popreg(Gen *generator, const char *register_name)
{
	emit(generator, "    pop %s", register_name);
	generator->tempdepth -= 8;
}

static void load_rax(Gen *generator, CType *type)
{
	if(type->kind == TY_ARRAY || type->kind == TY_STRUCT ||
	   type->kind == TY_XEVENT || type->kind == TY_VALIST)
		return;
	if(type->kind == TY_CHAR || type->kind == TY_U8)
		emit(generator, "    movzx eax, byte ptr [rax]");
	else if(type->kind == TY_SHORT || type->kind == TY_U16)
		emit(generator, "    movzx eax, word ptr [rax]");
	else if((type->kind == TY_INT || type->kind == TY_ENUM))
		emit(generator, "    movsxd rax, dword ptr [rax]");
	else if(type->kind == TY_U32 || type->kind == TY_FLOAT)
		emit(generator, "    mov eax, dword ptr [rax]");
	else
		emit(generator, "    mov rax, qword ptr [rax]");
}

static void store_rcx(Gen *generator, CType *type)
{
	if(type->kind == TY_CHAR || type->kind == TY_U8)
		emit(generator, "    mov byte ptr [rcx], al");
	else if(type->kind == TY_SHORT || type->kind == TY_U16)
		emit(generator, "    mov word ptr [rcx], ax");
	else if((type->kind == TY_INT || type->kind == TY_ENUM) || type->kind == TY_U32 || type->kind == TY_FLOAT)
		emit(generator, "    mov dword ptr [rcx], eax");
	else
		emit(generator, "    mov qword ptr [rcx], rax");
}

static bool is_double_type(CType *type)
{
	return type && type->kind == TY_DOUBLE;
}

static void integer_bits_to_double(Gen *generator)
{
	emit(generator, "    cvtsi2sd xmm0, rax");
	emit(generator, "    movq rax, xmm0");
}

static void gen_expr_as_double(Gen *generator, Expr *expression)
{
	CType *source = expr_type(generator, expression);
	gen_expr(generator, expression);
	if(!is_double_type(source))
		integer_bits_to_double(generator);
}

static void load_extern_global(Gen *generator, const char *name, CType *type)
{
	if(type->kind == TY_CHAR || type->kind == TY_U8)
		emit(generator, "    movzx eax, byte ptr [rip+%s]", name);
	else if(type->kind == TY_SHORT || type->kind == TY_U16)
		emit(generator, "    movzx eax, word ptr [rip+%s]", name);
	else if((type->kind == TY_INT || type->kind == TY_ENUM))
		emit(generator, "    movsxd rax, dword ptr [rip+%s]", name);
	else if(type->kind == TY_U32 || type->kind == TY_FLOAT)
		emit(generator, "    mov eax, dword ptr [rip+%s]", name);
	else
		emit(generator, "    mov rax, qword ptr [rip+%s]", name);
}

static CType *gen_addr(Gen *generator, Expr *expression)
{
	CType *type_1;
	if(expression->kind == EX_ID) {
		Symbol s = lookup(generator, expression->str);
		if(s.kind == SY_LOCAL) {
			emit(generator, "    lea rax, [rbp-%ld]", s.off);
			return s.type;
		}
		if(s.kind == SY_GLOBAL) {
			emit(generator, "    lea rax, [rip+%s]", s.name);
			return s.type;
		}
		fatal("%s is not assignable", expression->str);
	}
	if(expression->kind == EX_UNARY && !strcmp(expression->op, "*")) {
		gen_expr(generator, expression->left);
		type_1 = expr_type(generator, expression->left);
		return type_1->base ? type_1->base : &T_U64;
	}
	if(expression->kind == EX_INDEX) {
		gen_expr(generator, expression->left);
		pushreg(generator, "rax");
		gen_expr(generator, expression->right);
		type_1 = expr_type(generator, expression->left);
		type_1 = type_1->base ? type_1->base : &T_U64;
		if(type_size(type_1) != 1)
			emit(generator, "    imul rax, %ld", type_size(type_1));
		popreg(generator, "rcx");
		emit(generator, "    add rax, rcx");
		return type_1;
	}
	if(expression->kind == EX_MEMBER || expression->kind == EX_PTRMEMBER) {
		StructMember *member;
		CType *owner;
		if(expression->kind == EX_MEMBER) {
			gen_addr(generator, expression->left);
			owner = expr_type(generator, expression->left);
		} else {
			gen_expr(generator, expression->left);
			owner = expr_type(generator, expression->left);
			owner = owner && owner->kind == TY_PTR ? owner->base : NULL;
		}
		member = find_struct_member(owner, expression->str);
		if(!member)
			fatal("unknown struct member %s", expression->str);
		if(member->offset)
			emit(generator, "    add rax, %ld", member->offset);
		return member->type;
	}
	fatal("expression is not an lvalue");
	return &T_U64;
}

static CType *type_operand(Gen *generator, Expr *expression)
{
	if(!expression)
		return NULL;
	if(expression->kind == EX_TYPE)
		return expression->type;
	if(expression->kind == EX_TYPEOF)
		return expression->sizeof_type ? expression->sizeof_type : expr_type(generator, expression->left);
	return NULL;
}

static void gen_binary(Gen *generator, Expr *expression)
{
	const char *operator = expression->op;
	CType *left_type = type_operand(generator, expression->left);
	CType *right_type = type_operand(generator, expression->right);
	if((!strcmp(operator, "==") || !strcmp(operator, "!=")) && left_type && right_type) {
		bool equal = type_equal(left_type, right_type);
		emit(generator, "    mov eax, %d", !strcmp(operator, "==") ? equal : !equal);
		return;
	}
	CType *type;
	bool floating = is_double_type(expr_type(generator, expression->left)) ||
			is_double_type(expr_type(generator, expression->right));
	if(!strcmp(operator, "=")) {
		type = gen_addr(generator, expression->left);
		pushreg(generator, "rax");
		if(is_double_type(type))
			gen_expr_as_double(generator, expression->right);
		else {
			gen_expr(generator, expression->right);
			if(is_double_type(expr_type(generator, expression->right))) {
				emit(generator, "    movq xmm0, rax");
				emit(generator, "    cvttsd2si rax, xmm0");
			}
		}
		popreg(generator, "rcx");
		store_rcx(generator, type);
		return;
	}
	if(!strcmp(operator, "+=") || !strcmp(operator, "-=") || !strcmp(operator, "*=") || !strcmp(operator, "/=")) {
		type = gen_addr(generator, expression->left);
		pushreg(generator, "rax");
		load_rax(generator, type);
		if(is_double_type(type)) {
			pushreg(generator, "rax");
			gen_expr_as_double(generator, expression->right);
			popreg(generator, "rcx");
			emit(generator, "    movq xmm0, rcx");
			emit(generator, "    movq xmm1, rax");
			if(!strcmp(operator, "+="))
				emit(generator, "    addsd xmm0, xmm1");
			else if(!strcmp(operator, "-="))
				emit(generator, "    subsd xmm0, xmm1");
			else if(!strcmp(operator, "*="))
				emit(generator, "    mulsd xmm0, xmm1");
			else
				emit(generator, "    divsd xmm0, xmm1");
			emit(generator, "    movq rax, xmm0");
		} else {
			pushreg(generator, "rax");
			gen_expr(generator, expression->right);
			popreg(generator, "rcx");
			if(!strcmp(operator, "+="))
				emit(generator, "    add rax, rcx");
			else if(!strcmp(operator, "-=")) {
				emit(generator, "    sub rcx, rax");
				emit(generator, "    mov rax, rcx");
			} else if(!strcmp(operator, "*="))
				emit(generator, "    imul rax, rcx");
			else {
				emit(generator, "    mov r10, rax");
				emit(generator, "    mov rax, rcx");
				emit(generator, "    cqo");
				emit(generator, "    idiv r10");
			}
		}
		popreg(generator, "rcx");
		store_rcx(generator, type);
		return;
	}
	if(!strcmp(operator, "&&") || !strcmp(operator, "||")) {
		char *array = new_label(generator, ".Llogic"), *value = new_label(generator, ".Llogicdone");
		gen_expr(generator, expression->left);
		emit(generator, "    test rax, rax");
		if(!strcmp(operator, "&&"))
			emit(generator, "    jz %s", array);
		else
			emit(generator, "    jnz %s", array);
		gen_expr(generator, expression->right);
		emit(generator, "    test rax, rax");
		emit(generator, "    setne al");
		emit(generator, "    movzx rax, al");
		emit(generator, "    jmp %s", value);
		emit(generator, "%s:", array);
		if(!strcmp(operator, "&&"))
			emit(generator, "    xor eax, eax");
		else
			emit(generator, "    mov eax, 1");
		emit(generator, "%s:", value);
		return;
	}
	if(floating && (!strcmp(operator, "+") || !strcmp(operator, "-") || !strcmp(operator, "*") || !strcmp(operator, "/"))) {
		gen_expr_as_double(generator, expression->left);
		pushreg(generator, "rax");
		gen_expr_as_double(generator, expression->right);
		popreg(generator, "rcx");
		emit(generator, "    movq xmm0, rcx");
		emit(generator, "    movq xmm1, rax");
		if(!strcmp(operator, "+"))
			emit(generator, "    addsd xmm0, xmm1");
		else if(!strcmp(operator, "-"))
			emit(generator, "    subsd xmm0, xmm1");
		else if(!strcmp(operator, "*"))
			emit(generator, "    mulsd xmm0, xmm1");
		else
			emit(generator, "    divsd xmm0, xmm1");
		emit(generator, "    movq rax, xmm0");
		return;
	}
	gen_expr(generator, expression->left);
	pushreg(generator, "rax");
	gen_expr(generator, expression->right);
	popreg(generator, "rcx");
	if(!strcmp(operator, "+"))
		emit(generator, "    add rax, rcx");
	else if(!strcmp(operator, "-")) {
		emit(generator, "    sub rcx, rax");
		emit(generator, "    mov rax, rcx");
	} else if(!strcmp(operator, "*"))
		emit(generator, "    imul rax, rcx");
	else if(!strcmp(operator, "/") || !strcmp(operator, "%")) {
		emit(generator, "    mov r10, rax");
		emit(generator, "    mov rax, rcx");
		emit(generator, "    cqo");
		emit(generator, "    idiv r10");
		if(!strcmp(operator, "%"))
			emit(generator, "    mov rax, rdx");
	} else if(!strcmp(operator, "&"))
		emit(generator, "    and rax, rcx");
	else if(!strcmp(operator, "|"))
		emit(generator, "    or rax, rcx");
	else if(!strcmp(operator, "^"))
		emit(generator, "    xor rax, rcx");
	else if(!strcmp(operator, "<<") || !strcmp(operator, ">>")) {
		emit(generator, "    mov rdx, rax");
		emit(generator, "    mov rax, rcx");
		emit(generator, "    mov rcx, rdx");
		emit(generator, !strcmp(operator, "<<") ? "    shl rax, cl" : "    sar rax, cl");
	} else if(!strcmp(operator, "==") || !strcmp(operator, "!=") || !strcmp(operator, "<") || !strcmp(operator, "<=") || !strcmp(operator, ">") || !strcmp(operator, ">="))
	{
		const char *condition_code = !strcmp(operator, "==") ? "e" : !strcmp(operator, "!=") ? "ne"
						   : !strcmp(operator, "<")	     ? "l"
						   : !strcmp(operator, "<=")	     ? "le"
						   : !strcmp(operator, ">")	     ? "g"
									     : "ge";
		emit(generator, "    cmp rcx, rax");
		emit(generator, "    set%s al", condition_code);
		emit(generator, "    movzx rax, al");
	} else
		fatal("unsupported operator %s", operator);
}

static const char *call_name(const char *argument_count)
{
	if(!strcmp(argument_count, "DefaultScreen"))
		return "XDefaultScreen";
	if(!strcmp(argument_count, "RootWindow"))
		return "XRootWindow";
	if(!strcmp(argument_count, "BlackPixel"))
		return "XBlackPixel";
	if(!strcmp(argument_count, "WhitePixel"))
		return "XWhitePixel";
	return argument_count;
}

static void gen_input(Gen *generator, Expr *expression)
{
	char *no_newline_label = new_label(generator, ".Linput_no_newline");
	if(expression->nargs != 1)
		fatal("input expects one argument");
	generator->input_used = true;
	gen_expr(generator, expression->args[0]);
	emit(generator, "    mov rdi, rax");
	emit(generator, "    xor eax, eax");
	emit(generator, "    call printf@PLT");
	emit(generator, "    lea rax, [rip+__modulon_input_buffer]");
	emit(generator, "    mov rdi, rax");
	emit(generator, "    mov rax, 1024");
	emit(generator, "    mov rsi, rax");
	emit(generator, "    mov rax, qword ptr [rip+stdin]");
	emit(generator, "    mov rdx, rax");
	emit(generator, "    xor eax, eax");
	emit(generator, "    call fgets@PLT");
	emit(generator, "    lea rax, [rip+__modulon_input_buffer]");
	emit(generator, "    mov rdi, rax");
	emit(generator, "    mov rax, 10");
	emit(generator, "    mov rsi, rax");
	emit(generator, "    xor eax, eax");
	emit(generator, "    call strchr@PLT");
	emit(generator, "    test rax, rax");
	emit(generator, "    jz %s", no_newline_label);
	emit(generator, "    mov byte ptr [rax], 0");
	emit(generator, "%s:", no_newline_label);
	emit(generator, "    lea rax, [rip+__modulon_input_buffer]");
}

static const char *variable_format(CType *type)
{
	if(type && type->kind == TY_PTR && type->base && type->base->kind == TY_CHAR)
		return "%s";
	if(!type)
		fatal("cannot determine variable type for %%v");
	switch(type->kind) {
	case TY_BOOL:
	case TY_CHAR:
	case TY_SHORT:
	case TY_ENUM:
	case TY_INT:
	case TY_U8:
	case TY_U16:
	case TY_U32:
		return "%d";
	case TY_LONG:
		return "%ld";
	case TY_LLONG:
	case TY_I64:
		return "%lld";
	case TY_ULONG:
		return "%lu";
	case TY_U64:
		return "%llu";
	case TY_DOUBLE:
		return "%f";
	case TY_PTR:
		return "%p";
	default:
		fatal("unsupported type for %%v");
	}
	return "%d";
}

static void expand_variable_formats(Gen *generator, Expr *expression)
{
	const char *format;
	char *expanded;
	size_t index, output_index = 0, variable_index = 1, length;
	if(expression->nargs == 0 || expression->args[0]->kind != EX_STR)
		return;
	format = expression->args[0]->str;
	length = strlen(format);
	expanded = xmalloc(length + expression->nargs * 4 + 1);
	for(index = 0; index < length; index++) {
		if(format[index] == '%' && index + 1 < length && format[index + 1] == 'v') {
			if(variable_index >= expression->nargs)
				fatal("not enough variables for %%v");
			{
				const char *replacement = variable_format(expr_type(generator, expression->args[variable_index++]));
				while(*replacement)
					expanded[output_index++] = *replacement++;
			}
			index++;
		} else
			expanded[output_index++] = format[index];
	}
	expanded[output_index] = 0;
	expression->args[0]->str = expanded;
}

static void gen_call(Gen *generator, Expr *expression)
{
	size_t index, argument_count = expression->nargs;
	long nstack, pad, cleanup;
	const char *name;
	static const char *regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
	bool has_double = false;
	if(expression->left->kind != EX_ID)
		fatal("only direct calls supported");
	name = expression->left->str;
	if(!strcmp(name, "input")) {
		gen_input(generator, expression);
		return;
	}
	if(!strcmp(name, "va_start")) {
		Symbol ap;
		long named;
		if(!generator->current || !generator->current->variadic || argument_count != 2 || expression->args[0]->kind != EX_ID)
			fatal("bad va_start");
		ap = lookup(generator, expression->args[0]->str);
		named = (long)(generator->current->nparams < 6 ? generator->current->nparams : 6);
		emit(generator, "    lea rax, [rbp-%ld]", ap.off);
		emit(generator, "    mov dword ptr [rax], %ld", named * 8);
		emit(generator, "    mov dword ptr [rax+4], 48");
		emit(generator, "    lea rcx, [rbp+16]");
		emit(generator, "    mov qword ptr [rax+8], rcx");
		emit(generator, "    lea rcx, [rbp-%ld]", generator->vaoff);
		emit(generator, "    mov qword ptr [rax+16], rcx");
		emit(generator, "    xor eax, eax");
		return;
	}
	if(!strcmp(name, "va_end")) {
		emit(generator, "    xor eax, eax");
		return;
	}
	for(index = 0; index < argument_count; index++)
		if(is_double_type(expr_type(generator, expression->args[index])))
			has_double = true;
	if(!strcmp(name, "printf"))
		expand_variable_formats(generator, expression);
	name = call_name(name);
	if(has_double) {
		size_t integer_count = 0, double_count = 0;
		for(index = 0; index < argument_count; index++) {
			if(is_double_type(expr_type(generator, expression->args[index])))
				double_count++;
			else
				integer_count++;
		}
		if(integer_count > 6 || double_count > 8)
			fatal("native calls with floating arguments currently support 6 integer and 8 double register arguments");
		pad = (16 - ((generator->tempdepth + (long)argument_count * 8) % 16)) % 16;
		if(pad) {
			emit(generator, "    sub rsp, %ld", pad);
			generator->tempdepth += pad;
		}
		for(index = argument_count; index > 0; index--) {
			if(is_double_type(expr_type(generator, expression->args[index - 1])))
				gen_expr_as_double(generator, expression->args[index - 1]);
			else
				gen_expr(generator, expression->args[index - 1]);
			pushreg(generator, "rax");
		}
		integer_count = double_count = 0;
		for(index = 0; index < argument_count; index++) {
			if(is_double_type(expr_type(generator, expression->args[index]))) {
				popreg(generator, "rax");
				emit(generator, "    movq xmm%zu, rax", double_count++);
			} else {
				popreg(generator, regs[integer_count++]);
			}
		}
		emit(generator, "    mov eax, %zu", double_count);
		emit(generator, "    call %s@PLT", name);
		if(pad) {
			emit(generator, "    add rsp, %ld", pad);
			generator->tempdepth -= pad;
		}
		if(is_double_type(expr_type(generator, expression)))
			emit(generator, "    movq rax, xmm0");
		return;
	}
	nstack = (long)(argument_count > 6 ? argument_count - 6 : 0);
	pad = (16 - ((generator->tempdepth + nstack * 8) % 16)) % 16;
	if(pad) {
		emit(generator, "    sub rsp, %ld", pad);
		generator->tempdepth += pad;
	}
	for(index = argument_count; index > 0; index--) {
		gen_expr(generator, expression->args[index - 1]);
		pushreg(generator, "rax");
	}
	for(index = 0; index < argument_count && index < 6; index++)
		popreg(generator, regs[index]);
	emit(generator, "    xor eax, eax");
	emit(generator, "    call %s@PLT", name);
	cleanup = nstack * 8 + pad;
	if(cleanup) {
		emit(generator, "    add rsp, %ld", cleanup);
		generator->tempdepth -= cleanup;
	}
	if(is_double_type(expr_type(generator, expression)))
		emit(generator, "    movq rax, xmm0");
}

static void gen_incdec(Gen *generator, Expr *expression)
{
	CType *type = gen_addr(generator, expression->left);
	long step = 1;
	bool postfix = !strcmp(expression->op, "post++") ||
		       !strcmp(expression->op, "post--");
	bool decrement = !strcmp(expression->op, "--") ||
			 !strcmp(expression->op, "post--");
	if(type->kind == TY_PTR && type->base) {
		step = type_size(type->base);
		if(step <= 0)
			step = 1;
	}
	pushreg(generator, "rax");
	load_rax(generator, type);
	if(postfix)
		pushreg(generator, "rax");
	if(decrement)
		emit(generator, "    sub rax, %ld", step);
	else
		emit(generator, "    add rax, %ld", step);
	if(postfix) {
		popreg(generator, "rdx");
		popreg(generator, "rcx");
		store_rcx(generator, type);
		emit(generator, "    mov rax, rdx");
	} else {
		popreg(generator, "rcx");
		store_rcx(generator, type);
	}
}

static void gen_stmt(Gen *generator, Stmt *statement);

static bool match_unsigned(CType *type)
{
	return type && (type->kind == TY_U8 || type->kind == TY_U16 ||
			type->kind == TY_U32 || type->kind == TY_U64 ||
			type->kind == TY_ULONG);
}

static void gen_match_condition(Gen *generator, CType *subject_type, MatchArm *arm,
				const char *success_label, const char *next_label)
{
	size_t index;
	const char *below = match_unsigned(subject_type) ? "b" : "l";
	const char *above = match_unsigned(subject_type) ? "a" : "g";
	if(is_double_type(subject_type))
		fatal("match does not support double values");
	for(index = 0; index < arm->npatterns; index++) {
		char *pattern_next = new_label(generator, ".Lmatchpattern");
		if(is_double_type(expr_type(generator, arm->patterns[index])))
			fatal("match patterns cannot be double values");
		gen_expr(generator, arm->patterns[index]);
		popreg(generator, "rcx");
		emit(generator, "    cmp rcx, rax");
		if(arm->pattern_ranges[index]) {
			pushreg(generator, "rcx");
			emit(generator, "    j%s %s", below, pattern_next);
			gen_expr(generator, arm->pattern_highs[index]);
			popreg(generator, "rcx");
			emit(generator, "    cmp rcx, rax");
			pushreg(generator, "rcx");
			emit(generator, "    j%s %s", above, pattern_next);
			emit(generator, "    jmp %s", success_label);
		} else {
			pushreg(generator, "rcx");
			emit(generator, "    jz %s", success_label);
		}
		emit(generator, "%s:", pattern_next);
	}
	if(arm->is_default)
		emit(generator, "    jmp %s", success_label);
	else
		emit(generator, "    jmp %s", next_label);
}

static void gen_match_expr(Gen *generator, Expr *expression)
{
	size_t index;
	char *success_label;
	char *next_label;
	char *end_label = new_label(generator, ".Lmatchend");
	gen_expr(generator, expression->left);
	pushreg(generator, "rax");
	for(index = 0; index < expression->narms; index++) {
		success_label = new_label(generator, ".Lmatcharm");
		next_label = new_label(generator, ".Lmatchnext");
		gen_match_condition(generator, expr_type(generator, expression->left), &expression->arms[index], success_label, next_label);
		emit(generator, "%s:", success_label);
		if(expression->arms[index].guard) {
			gen_expr(generator, expression->arms[index].guard);
			emit(generator, "    test rax, rax");
			emit(generator, "    jz %s", next_label);
		}
		popreg(generator, "rcx");
		gen_expr(generator, expression->arms[index].expr);
		emit(generator, "    jmp %s", end_label);
		emit(generator, "%s:", next_label);
	}
	popreg(generator, "rcx");
	emit(generator, "    xor eax, eax");
	emit(generator, "%s:", end_label);
}

static void gen_match_stmt(Gen *generator, Stmt *statement)
{
	size_t index;
	char *success_label;
	char *next_label;
	char *end_label = new_label(generator, ".Lmatchend");
	gen_expr(generator, statement->expr);
	pushreg(generator, "rax");
	ARR_GROW(generator->breaks, generator->nbreaks, generator->capbreaks, char *);
	generator->breaks[generator->nbreaks++] = end_label;
	for(index = 0; index < statement->narms; index++) {
		success_label = new_label(generator, ".Lmatcharm");
		next_label = new_label(generator, ".Lmatchnext");
		gen_match_condition(generator, expr_type(generator, statement->expr), &statement->arms[index], success_label, next_label);
		emit(generator, "%s:", success_label);
		if(statement->arms[index].guard) {
			gen_expr(generator, statement->arms[index].guard);
			emit(generator, "    test rax, rax");
			emit(generator, "    jz %s", next_label);
		}
		popreg(generator, "rcx");
		gen_stmt(generator, statement->arms[index].stmt);
		emit(generator, "    jmp %s", end_label);
		emit(generator, "%s:", next_label);
	}
	popreg(generator, "rcx");
	emit(generator, "%s:", end_label);
	generator->nbreaks--;
}

static void gen_expr(Gen *generator, Expr *expression)
{
	CType *type_1;
	char *lab;
	Symbol s;
	switch(expression->kind) {
	case EX_ENUM_CONST:
	case EX_NUM:
		if(is_double_type(expression->type)) {
			union
			{
				double d;
				uint64_t u;
			} bits;
			bits.d = expression->fnum;
			emit(generator, "    mov rax, %llu", (unsigned long long)bits.u);
		} else
			emit(generator, "    mov rax, %llu", expression->num);
		return;
	case EX_STR:
		lab = intern_string(generator, expression->str);
		emit(generator, "    lea rax, [rip+%s]", lab);
		return;
	case EX_ID:
		s = lookup(generator, expression->str);
		if(s.kind == SY_CONST) {
			emit(generator, "    mov rax, %ld", s.off);
			return;
		}
		if(s.kind == SY_EXTERN_GLOBAL) {
			load_extern_global(generator, s.name, s.type);
			return;
		}
		if(s.kind == SY_FUNCTION || s.kind == SY_EXTERN_FUNCTION) {
			emit(generator, "    lea rax, [rip+%s]", s.name);
			return;
		}
		type_1 = gen_addr(generator, expression);
		load_rax(generator, type_1);
		return;
	case EX_SIZEOF:
		emit(generator, "    mov rax, %ld", type_size(expression->sizeof_type ? expression->sizeof_type : expr_type(generator, expression->left)));
		return;
	case EX_TYPEOF:
	case EX_TYPE:
		fatal("type expression cannot be used as a runtime value");
		return;
	case EX_UNARY:
		if(!strcmp(expression->op, "++") || !strcmp(expression->op, "--") ||
		   !strcmp(expression->op, "post++") || !strcmp(expression->op, "post--"))
		{
			gen_incdec(generator, expression);
		} else if(!strcmp(expression->op, "cast"))
		{
			CType *source = expr_type(generator, expression->left);
			gen_expr(generator, expression->left);
			if(expression->type->kind == TY_DOUBLE) {
				if(!is_double_type(source))
					integer_bits_to_double(generator);
			} else {
				if(is_double_type(source)) {
					emit(generator, "    movq xmm0, rax");
					emit(generator, "    cvttsd2si rax, xmm0");
				}
				if(expression->type->kind == TY_CHAR || expression->type->kind == TY_U8)
					emit(generator, "    movzx eax, al");
				else if(expression->type->kind == TY_SHORT || expression->type->kind == TY_U16)
					emit(generator, "    movzx eax, ax");
				else if((expression->type->kind == TY_INT || expression->type->kind == TY_ENUM))
					emit(generator, "    movsxd rax, eax");
				else if(expression->type->kind == TY_U32)
					emit(generator, "    mov eax, eax");
			}
		} else if(!strcmp(expression->op, "&"))
			gen_addr(generator, expression->left);
		else if(!strcmp(expression->op, "*")) {
			gen_expr(generator, expression->left);
			load_rax(generator, expr_type(generator, expression));
		} else {
			gen_expr(generator, expression->left);
			if(!strcmp(expression->op, "!")) {
				emit(generator, "    test rax, rax");
				emit(generator, "    sete al");
				emit(generator, "    movzx rax, al");
			} else if(!strcmp(expression->op, "~"))
				emit(generator, "    not rax");
			else if(!strcmp(expression->op, "-"))
				emit(generator, "    neg rax");
		}
		return;
	case EX_INDEX:
	case EX_MEMBER:
	case EX_PTRMEMBER:
		type_1 = gen_addr(generator, expression);
		load_rax(generator, type_1);
		return;
	case EX_BINARY:
		gen_binary(generator, expression);
		return;
	case EX_CALL:
		gen_call(generator, expression);
		return;
	case EX_INITLIST:
		fatal("initializer list used as an expression");
		return;
	case EX_MATCH:
		gen_match_expr(generator, expression);
		return;
	}
	fatal("unsupported expression");
}

static long collect_locals(Gen *generator, Stmt *statement, long off)
{
	size_t index;
	Decl *declaration;
	if(!statement)
		return off;
	if(statement->kind == ST_DECL) {
		declaration = statement->decl;
		if(declaration->is_var) {
			if(!declaration->init)
				fatal("var declaration requires an initializer");
			declaration->type = expr_type(generator, declaration->init);
			if(!declaration->type)
				fatal("cannot infer type of var %s", declaration->name);
		}
		if(find_local(generator, declaration->name))
			fatal("duplicate local %s", declaration->name);
		off = align_up(off, type_align(declaration->type));
		off += type_size(declaration->type) > 0 ? type_size(declaration->type) : 1;
		ARR_GROW(generator->locals, generator->nlocals, generator->caplocals, Symbol);
		generator->locals[generator->nlocals++] = (Symbol){declaration->name, declaration->type, SY_LOCAL, off, NULL};
	} else if(statement->kind == ST_BLOCK)
		for(index = 0; index < statement->nchildren; index++)
			off = collect_locals(generator, statement->children[index], off);
	else if(statement->kind == ST_IF) {
		off = collect_locals(generator, statement->yes, off);
		off = collect_locals(generator, statement->no, off);
	} else if(statement->kind == ST_WHILE)
		off = collect_locals(generator, statement->body, off);
	else if(statement->kind == ST_FOR) {
		off = collect_locals(generator, statement->init, off);
		off = collect_locals(generator, statement->body, off);
	}
	else if(statement->kind == ST_MATCH)
		for(index = 0; index < statement->narms; index++)
			off = collect_locals(generator, statement->arms[index].stmt, off);
	return off;
}

static void gen_stmt(Gen *generator, Stmt *statement)
{
	size_t index;
	Symbol *x;
	char *array, *value;
	if(!statement)
		return;
	switch(statement->kind) {
	case ST_BLOCK:
		for(index = 0; index < statement->nchildren; index++)
			gen_stmt(generator, statement->children[index]);
		return;
	case ST_DECL:
		if(statement->decl->init) {
			x = find_local(generator, statement->decl->name);
			if(is_double_type(x->type))
				gen_expr_as_double(generator, statement->decl->init);
			else
				gen_expr(generator, statement->decl->init);
			emit(generator, "    lea rcx, [rbp-%ld]", x->off);
			store_rcx(generator, x->type);
		}
		return;
	case ST_EXPR:
		gen_expr(generator, statement->expr);
		return;
	case ST_EMPTY:
		return;
	case ST_RETURN:
		if(statement->expr) {
			if(generator->current && is_double_type(generator->current->type)) {
				gen_expr_as_double(generator, statement->expr);
				emit(generator, "    movq xmm0, rax");
			} else
				gen_expr(generator, statement->expr);
		} else
			emit(generator, "    xor eax, eax");
		emit(generator, "    jmp %s", generator->retlabel);
		return;
	case ST_IF:
		array = new_label(generator, ".Lelse");
		value = new_label(generator, ".Lifend");
		gen_expr(generator, statement->cond);
		emit(generator, "    test rax, rax");
		emit(generator, "    jz %s", array);
		gen_stmt(generator, statement->yes);
		emit(generator, "    jmp %s", value);
		emit(generator, "%s:", array);
		gen_stmt(generator, statement->no);
		emit(generator, "%s:", value);
		return;
	case ST_WHILE:
		array = new_label(generator, ".Lwhile");
		value = new_label(generator, ".Lwend");
		ARR_GROW(generator->breaks, generator->nbreaks, generator->capbreaks, char *);
		generator->breaks[generator->nbreaks++] = value;
		ARR_GROW(generator->continues, generator->ncontinues, generator->capcontinues, char *);
		generator->continues[generator->ncontinues++] = array;
		emit(generator, "%s:", array);
		gen_expr(generator, statement->cond);
		emit(generator, "    test rax, rax");
		emit(generator, "    jz %s", value);
		gen_stmt(generator, statement->body);
		emit(generator, "    jmp %s", array);
		emit(generator, "%s:", value);
		generator->nbreaks--;
		generator->ncontinues--;
		return;
	case ST_FOR:
	{
		char *character = new_label(generator, ".Lforcond");
		char *count = new_label(generator, ".Lfornext");
		value = new_label(generator, ".Lforend");
		gen_stmt(generator, statement->init);
		ARR_GROW(generator->breaks, generator->nbreaks, generator->capbreaks, char *);
		generator->breaks[generator->nbreaks++] = value;
		ARR_GROW(generator->continues, generator->ncontinues, generator->capcontinues, char *);
		generator->continues[generator->ncontinues++] = count;
		emit(generator, "%s:", character);
		if(statement->cond) {
			gen_expr(generator, statement->cond);
			emit(generator, "    test rax, rax");
			emit(generator, "    jz %s", value);
		}
		gen_stmt(generator, statement->body);
		emit(generator, "%s:", count);
		if(statement->post)
			gen_expr(generator, statement->post);
		emit(generator, "    jmp %s", character);
		emit(generator, "%s:", value);
		generator->nbreaks--;
		generator->ncontinues--;
		return;
	}
	case ST_MATCH:
		gen_match_stmt(generator, statement);
		return;
	case ST_SWITCH:
	case ST_CASE:
	case ST_DEFAULT:
		fatal("switch is supported by the i386 object backend only");
		return;
	case ST_BREAK:
		if(!generator->nbreaks)
			fatal("break outside loop");
		emit(generator, "    jmp %s", generator->breaks[generator->nbreaks - 1]);
		return;
	case ST_CONTINUE:
		if(!generator->ncontinues)
			fatal("continue outside loop");
		emit(generator, "    jmp %s", generator->continues[generator->ncontinues - 1]);
		return;
	case ST_ASM:
		if(!strcmp(statement->asm_text, "hlt") || !strcmp(statement->asm_text, "cli") ||
		   !strcmp(statement->asm_text, "sti") || !strcmp(statement->asm_text, "nop") ||
		   !strcmp(statement->asm_text, "cld") || !strcmp(statement->asm_text, "std") ||
		   !strcmp(statement->asm_text, "int3") || !strcmp(statement->asm_text, "pause") ||
		   !strcmp(statement->asm_text, "ud2"))
			emit(generator, "    %s", statement->asm_text);
		else
			fatal("unsupported inline asm instruction: %s", statement->asm_text);
		return;
	}
}

static void gen_function(Gen *generator, Decl *declaration)
{
	size_t index;
	long off = 0;
	static const char *regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
	Symbol *s;
	generator->current = declaration;
	generator->nlocals = 0;
	generator->tempdepth = 0;
	generator->vaoff = 0;
	for(index = 0; index < declaration->nparams; index++) {
		off = align_up(off, 8) + 8;
		ARR_GROW(generator->locals, generator->nlocals, generator->caplocals, Symbol);
		generator->locals[generator->nlocals++] = (Symbol){declaration->params[index].name, declaration->params[index].type, SY_LOCAL, off, NULL};
	}
	off = collect_locals(generator, declaration->body, off);
	if(declaration->variadic) {
		off = align_up(off, 16) + 176;
		generator->vaoff = off;
	}
	generator->frame = align_up(off, 16);
	snprintf(generator->retlabel, sizeof(generator->retlabel), ".Lreturn_%s_%ld", declaration->name, ++generator->label);
	emit(generator, ".text");
	if(!declaration->is_static)
		emit(generator, ".globl %s", declaration->name);
	emit(generator, ".type %s, @function", declaration->name);
	emit(generator, "%s:", declaration->name);
	emit(generator, "    push rbp");
	emit(generator, "    mov rbp, rsp");
	if(generator->frame)
		emit(generator, "    sub rsp, %ld", generator->frame);
	{
		size_t integer_parameter = 0;
		size_t double_parameter = 0;
		for(index = 0; index < declaration->nparams; index++) {
			s = find_local(generator, declaration->params[index].name);
			if(is_double_type(declaration->params[index].type)) {
				if(double_parameter >= 8)
					fatal("native functions currently support up to 8 double register parameters");
				emit(generator, "    movq rax, xmm%zu", double_parameter++);
				emit(generator, "    mov qword ptr [rbp-%ld], rax", s->off);
			} else if(integer_parameter < 6)
			{
				emit(generator, "    mov qword ptr [rbp-%ld], %s", s->off, regs[integer_parameter++]);
			} else {
				emit(generator, "    mov rax, qword ptr [rbp+%zu]", 16 + (integer_parameter - 6) * 8);
				emit(generator, "    mov qword ptr [rbp-%ld], rax", s->off);
				integer_parameter++;
			}
		}
	}
	if(declaration->variadic)
		for(index = 0; index < 6; index++)
			emit(generator, "    mov qword ptr [rbp-%ld], %s", generator->vaoff - (long)index * 8, regs[index]);
	gen_stmt(generator, declaration->body);
	emit(generator, "    xor eax, eax");
	emit(generator, "%s:", generator->retlabel);
	emit(generator, "    leave");
	emit(generator, "    ret");
	emit(generator, ".size %s, .-%s", declaration->name, declaration->name);
}

char *generate(Program *pointer)
{
	Gen g = {0};
	size_t index;
	Decl *declaration;
	g.prog = pointer;
	for(index = 0; index < pointer->n; index++) {
		declaration = pointer->a[index];
		if(declaration->body || declaration->prototype) {
			ARR_GROW(g.funcs, g.nfuncs, g.capfuncs, Decl *);
			g.funcs[g.nfuncs++] = declaration;
		} else {
			ARR_GROW(g.globals, g.nglobals, g.capglobals, Symbol);
			g.globals[g.nglobals++] = (Symbol){
				declaration->name, declaration->type, declaration->is_extern ? SY_EXTERN_GLOBAL : SY_GLOBAL, 0, NULL};
		}
	}
	emit(&g, ".intel_syntax noprefix");
	emit(&g, ".text");
	emit(&g, ".globl _modulon_init");
	emit(&g, "_modulon_init:");
	for(index = 0; index < pointer->n; index++) {
		declaration = pointer->a[index];
		if(declaration->body || declaration->prototype || !declaration->init)
			continue;
        if(declaration->type->kind == TY_ENUM ||
           (declaration->type->kind == TY_INT && declaration->init->kind == EX_ENUM_CONST)) {
            long value;
            if(!eval_const_expr(declaration->init,&value))
                fatal("enum initializer for %s is not constant",declaration->name);
            emit(&g,"    lea rax, [rip+%s]",declaration->name);
            emit(&g,"    mov rcx, rax");
            emit(&g,"    mov rax, %ld",value);
            store_rcx(&g,declaration->type);
        } else if(declaration->type->kind == TY_PTR && declaration->type->base &&
			declaration->type->base->kind == TY_CHAR &&
			declaration->init->kind == EX_STR) {
			char *label = intern_string(&g, declaration->init->str);
			emit(&g, "    lea rax, [rip+%s]", declaration->name);
			emit(&g, "    mov rcx, rax");
			emit(&g, "    lea rax, [rip+%s]", label);
			emit(&g, "    mov qword ptr [rcx], rax");
		}
	}
	emit(&g, "    ret");
	for(index = 0; index < pointer->n; index++)
		if(pointer->a[index]->body)
			gen_function(&g, pointer->a[index]);
	if(g.nglobals) {
		bool emitted_bss = false;
		for(index = 0; index < g.nglobals; index++) {
			Symbol *sym = &g.globals[index];
			if(sym->kind != SY_GLOBAL)
				continue;
			if(!emitted_bss) {
				emit(&g, ".bss");
				emitted_bss = true;
			}
			emit(&g, ".globl %s", sym->name);
			emit(&g, ".align %ld", type_align(sym->type));
			emit(&g, "%s:", sym->name);
			emit(&g, "    .zero %ld", type_size(sym->type) > 0 ? type_size(sym->type) : 1);
		}
	}
	if(g.input_used) {
		emit(&g, ".bss");
		emit(&g, ".align 16");
		emit(&g, "__modulon_input_buffer:");
		emit(&g, "    .zero 1024");
	}
	if(g.ro.n) {
		emit(&g, ".section .rodata");
		bputn(&g.out, g.ro.s, g.ro.n);
	}
	emit(&g, ".section .note.GNU-stack,\"\",@progbits");
	return g.out.s;
}
