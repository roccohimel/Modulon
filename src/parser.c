#include "parser.h"


static Token *ptok(Parser *parser)
{
	return &parser->ts->a[parser->p];
}

static bool peq(Parser *parser, const char *string)
{
	return !strcmp(ptok(parser)->v, string);
}

static bool paccept(Parser *parser, const char *string)
{
	if(peq(parser, string)) {
		parser->p++;
		return true;
	}
	return false;
}

static size_t recovery_position;
static int recovery_brace_depth;

static void sync_parser(Parser *parser)
{
	size_t position = recovery_position;
	int paren_depth = 0;
	int bracket_depth = 0;
	int brace_depth = recovery_brace_depth;
	while(position < parser->ts->n) {
		Token *token = &parser->ts->a[position];
		if(!strcmp(token->v, "("))
			paren_depth++;
		else if(!strcmp(token->v, ")")) {
			if(paren_depth > 0)
				paren_depth--;
		} else if(!strcmp(token->v, "["))
			bracket_depth++;
		else if(!strcmp(token->v, "]")) {
			if(bracket_depth > 0)
				bracket_depth--;
		} else if(!strcmp(token->v, "{"))
			brace_depth++;
		else if(!strcmp(token->v, "}")) {
			if(brace_depth > 0) {
				brace_depth--;
				if(brace_depth == 0) {
					position++;
					break;
				}
			} else if(paren_depth == 0 && bracket_depth == 0) {
				position++;
				break;
			}
		} else if(!strcmp(token->v, ";") &&
			  paren_depth == 0 && bracket_depth == 0 && brace_depth==0) {
			position++;
			break;
		}
		position++;
	}
	if(position<parser->ts->n && !strcmp(parser->ts->a[position].v,";")) position++;
	if(position >= parser->ts->n)
		position = parser->ts->n - 1;
	parser->p = position;
	parser->recovery_brace_depth = 0;
}

static void perr(Parser *parser, const char *fmt, ...)
{
	va_list argument_list;
	char message[1024];
	Token *token = ptok(parser);
	va_start(argument_list, fmt);
	vsnprintf(message, sizeof(message), fmt, argument_list);
	va_end(argument_list);
	diagnostic_report(DIAG_ERROR, parser->ts->file, parser->ts->source,
		token->line, token->col, "%s; got '%s'", message, token->v);
	recovery_position = parser->p;
	recovery_brace_depth = parser->recovery_brace_depth;
	longjmp(parser->error_jmp, 1);
}

static void pexpect(Parser *parser, const char *string)
{
	if(!paccept(parser, string))
		perr(parser, "expected '%s'", string);
}

static char *pexpect_id(Parser *parser, bool optional)
{
	if(ptok(parser)->kind == TK_ID)
		return parser->ts->a[parser->p++].v;
	if(optional)
		return xstrdup("");
	perr(parser, "expected identifier");
	return NULL;
}

static CType *find_alias(Program *prog, const char *name)
{
	size_t index;
	for(index = 0; index < prog->naliases; index++)
		if(!strcmp(prog->aliases[index].name, name))
			return prog->aliases[index].type;
	return NULL;
}

static bool same_type(CType *array, CType *base_type)
{
	if(array == base_type)
		return true;
	if(!array || !base_type || array->kind != base_type->kind || array->count != base_type->count)
		return false;
	if(array->kind == TY_ENUM)
		return array == base_type;
	if(array->kind == TY_PTR || array->kind == TY_ARRAY)
		return same_type(array->base, base_type->base);
	return true;
}

static void add_alias(Parser *parser, char *name, CType *type)
{
	Program *prog = parser->prog;
	CType *old = find_alias(prog, name);
	if(old) {
		if(same_type(old, type))
			return;
		perr(parser, "conflicting typedef '%s'", name);
	}
	ARR_GROW(prog->aliases, prog->naliases, prog->capaliases, TypeAlias);
	prog->aliases[prog->naliases].name = name;
	prog->aliases[prog->naliases].type = type;
	prog->naliases++;
}

static CType *find_struct_tag(Program *prog, const char *name)
{
	size_t index;
    index=prog->ntags;
    while(index--) {
        if(prog->tags[index].active && !strcmp(prog->tags[index].name,name))
            return prog->tags[index].type;
    }
    return NULL;
}

static int struct_tag_depth(Program *prog, const char *name)
{
    size_t i=prog->ntags;
    while(i--) if(prog->tags[i].active && !strcmp(prog->tags[i].name,name))
        return prog->tags[i].depth;
    return -1;
}

static void add_struct_tag(Parser *parser, char *name, CType *type)
{
	CType *old;
	if(!name || !*name)
		return;
	old = find_struct_tag(parser->prog, name);
    if(struct_tag_depth(parser->prog,name)!=parser->scope_depth) old=NULL;
	if(old && old != type)
		perr(parser, "conflicting struct tag '%s'", name);
	if(old)
		return;
	ARR_GROW(parser->prog->tags, parser->prog->ntags, parser->prog->captags, StructTag);
	parser->prog->tags[parser->prog->ntags].name = name;
	parser->prog->tags[parser->prog->ntags].type = type;
    parser->prog->tags[parser->prog->ntags].depth=parser->scope_depth;
    parser->prog->tags[parser->prog->ntags].active=true;
	parser->prog->ntags++;
}

StructMember *find_struct_member(CType *type, const char *name)
{
	static StructMember xevent_type_member = {"type", &T_INT, 0};
	size_t index;
	if(!type)
		return NULL;
	if(type->kind == TY_XEVENT)
		return !strcmp(name, "type") ? &xevent_type_member : NULL;
	if(type->kind != TY_STRUCT)
		return NULL;
	for(index = 0; index < type->nmembers; index++)
		if(!strcmp(type->members[index].name, name))
			return &type->members[index];
	return NULL;
}
static Declarator parse_declarator(Parser *p, CType *base, bool unnamed);
static Expr *parse_expr(Parser *p, int minprec);
bool eval_const_expr(Expr *e, long *value);

static bool type_start(Parser *parser)
{
	const char *character = ptok(parser)->v;
	if(find_alias(parser->prog, character))
		return true;
	return !strcmp(character, "void") || !strcmp(character, "_Bool") ||
	       !strcmp(character, "bool") || !strcmp(character, "string") || !strcmp(character, "char") ||
	       !strcmp(character, "short") || !strcmp(character, "int") ||
	       !strcmp(character, "long") || !strcmp(character, "float") ||
	       !strcmp(character, "double") || !strcmp(character, "signed") ||
	       !strcmp(character, "unsigned") || !strcmp(character, "const") ||
	       !strcmp(character, "volatile") || !strcmp(character, "restrict") ||
	       !strcmp(character, "struct") || !strcmp(character, "enum") ||
	       !strcmp(character, "FILE") || !strcmp(character, "Display") ||
	       !strcmp(character, "Window") || !strcmp(character, "GC") ||
	       !strcmp(character, "Font") || !strcmp(character, "XEvent") ||
	       !strcmp(character, "va_list") || !strcmp(character, "size_t") ||
	       !strcmp(character, "ssize_t") || !strcmp(character, "ptrdiff_t") ||
	       !strcmp(character, "intptr_t") || !strcmp(character, "uintptr_t") ||
	       !strcmp(character, "intmax_t") || !strcmp(character, "uintmax_t") ||
	       !strcmp(character, "int8_t") || !strcmp(character, "int16_t") ||
	       !strcmp(character, "int32_t") || !strcmp(character, "int64_t") ||
	       !strcmp(character, "uint8_t") || !strcmp(character, "uint16_t") ||
	       !strcmp(character, "uint32_t") || !strcmp(character, "uint64_t") ||
	       !strcmp(character, "int_least8_t") || !strcmp(character, "int_least16_t") ||
	       !strcmp(character, "int_least32_t") || !strcmp(character, "int_least64_t") ||
	       !strcmp(character, "uint_least8_t") || !strcmp(character, "uint_least16_t") ||
	       !strcmp(character, "uint_least32_t") || !strcmp(character, "uint_least64_t") ||
	       !strcmp(character, "int_fast8_t") || !strcmp(character, "int_fast16_t") ||
	       !strcmp(character, "int_fast32_t") || !strcmp(character, "int_fast64_t") ||
	       !strcmp(character, "uint_fast8_t") || !strcmp(character, "uint_fast16_t") ||
	       !strcmp(character, "uint_fast32_t") || !strcmp(character, "uint_fast64_t");
}

static Expr *parse_expr(Parser *parser, int minprec);

/* Ordinary identifiers and enum tags have separate, lexical namespaces. */
static NameBinding *find_binding(Parser *p, const char *name)
{
    size_t i = p->prog->nbindings;
    while(i--) {
        NameBinding *b = &p->prog->bindings[i];
        if(b->active && !strcmp(b->name, name)) return b;
    }
    return NULL;
}

static void bind_name(Parser *p, char *name, CType *type, bool constant,
                      long value, bool function)
{
    NameBinding *old = find_binding(p, name);
    if(old && old->depth == p->scope_depth &&
       (old->is_enum_constant || constant))
        perr(p, "duplicate enum constant or conflicting identifier '%s'", name);
    if(constant && find_alias(p->prog, name))
        perr(p, "enum constant '%s' conflicts with typedef", name);
    ARR_GROW(p->prog->bindings, p->prog->nbindings,
             p->prog->capbindings, NameBinding);
    p->prog->bindings[p->prog->nbindings++] = (NameBinding){
        name, type, value, p->scope_depth, true, constant, function};
}

static void leave_scope(Parser *p)
{
    size_t i;
    for(i=0; i<p->prog->nbindings; i++)
        if(p->prog->bindings[i].depth == p->scope_depth)
            p->prog->bindings[i].active = false;
    for(i=0; i<p->prog->ntags; i++)
        if(p->prog->tags[i].depth == p->scope_depth) p->prog->tags[i].active=false;
    for(i=0; i<p->prog->nenum_tags; i++)
        if(p->prog->enum_tags[i].depth == p->scope_depth)
            p->prog->enum_tags[i].active = false;
    p->scope_depth--;
}

static EnumTag *find_enum_tag(Parser *p, const char *name)
{
    size_t i=p->prog->nenum_tags;
    while(i--) {
        EnumTag *tag=&p->prog->enum_tags[i];
        if(tag->active && !strcmp(tag->name,name)) return tag;
    }
    return NULL;
}

static bool reserved_enum_name(const char *name)
{
    static const char *words[]={"if","else","while","for","return","break",
        "continue","switch","case","default","typedef","extern","static",
        "inline","sizeof","typeof","match","var","do","goto","union","auto",
        "register","_Alignof","_Alignas","_Static_assert","_Thread_local",
        "_Noreturn","_Generic","_Atomic","_Complex","_Imaginary",NULL};
    size_t i;
    static const char *types[]={"int","char","short","long","float","double","void","enum","struct","bool","_Bool","string","signed","unsigned","const","volatile","restrict",NULL};
    for(i=0;types[i];i++) if(!strcmp(name,types[i])) return true;
    for(i=0; words[i]; i++) if(!strcmp(name,words[i])) return true;
    return false;
}

static CType *parse_enum(Parser *p)
{
    char *name=NULL;
    CType *type;
    EnumTag *existing;
    long next=0;
    bool next_overflow=false;
    if(ptok(p)->kind==TK_ID) name=pexpect_id(p,false);
    existing=name ? find_enum_tag(p,name) : NULL;
    if(!paccept(p,"{")) {
        if(name && existing && struct_tag_depth(p->prog,name)>=existing->depth)
            perr(p,"tag '%s' names a struct, not an enum",name);
        if(!name || !existing || !existing->type->enum_complete)
            perr(p,"unknown or incomplete enum tag '%s'",name ? name : "<missing>");
        return existing->type;
    }
    if(name && reserved_enum_name(name)) perr(p,"invalid enum tag '%s'",name);
    if(existing && existing->depth==p->scope_depth)
        perr(p,"redefinition of enum '%s'",name);
    if(name && struct_tag_depth(p->prog,name)==p->scope_depth)
        perr(p,"enum tag '%s' conflicts with struct tag",name);
    p->recovery_brace_depth++;
    type=new_type(TY_ENUM,NULL,0,name ? name : "<anonymous enum>");
    ARR_GROW(p->prog->enum_types,p->prog->nenum_types,p->prog->capenum_types,CType *);
    p->prog->enum_types[p->prog->nenum_types++]=type;
    if(name) {
        ARR_GROW(p->prog->enum_tags,p->prog->nenum_tags,p->prog->capenum_tags,EnumTag);
        p->prog->enum_tags[p->prog->nenum_tags++]=(EnumTag){name,type,p->scope_depth,true};
    }
    if(peq(p,"}")) perr(p,"enum declaration must contain at least one enumerator");
    for(;;) {
        char *member;
        long value=next;
        Expr *initializer;
        if(ptok(p)->kind!=TK_ID)
            perr(p,"expected enumerator name in enum declaration");
        member=pexpect_id(p,false);
        if(reserved_enum_name(member)) perr(p,"invalid enumerator name '%s'",member);
        if(!peq(p,"=") && next_overflow)
            perr(p,"implicit enum value for '%s' overflows signed 32-bit range",member);
        if(paccept(p,"=")) {
            initializer=parse_expr(p,2);
            if(!eval_const_expr(initializer,&value))
                perr(p,"enum value for '%s' must be a valid integer constant expression (check unknown names, division by zero, or invalid shifts)",member);
        }
        if(value<INT32_MIN || value>INT32_MAX)
            perr(p,"enum value for '%s' is outside signed 32-bit range",member);
        bind_name(p,member,&T_INT,true,value,false);
        ARR_GROW(type->enumerators,type->nenumerators,type->capenumerators,EnumMember);
        type->enumerators[type->nenumerators++]=(EnumMember){member,(int32_t)value};
        next_overflow=value==INT32_MAX;
        next=next_overflow ? 0 : value+1;
        if(paccept(p,"}")) break;
        if(!paccept(p,",")) perr(p,"expected ',' or '}' after enum enumerator '%s'",member);
        if(paccept(p,"}")) break;
    }
    p->recovery_brace_depth--;
    type->enum_complete=true;
    p->enum_definition=true;
    return type;
}

static CType *parsed_expr_type(Expr *e)
{
    CType *t;
    if(!e) return NULL;
    if(e->type) return e->type;
    switch(e->kind) {
    case EX_UNARY:
        t=parsed_expr_type(e->left);
        if(!strcmp(e->op,"&")) return t ? ptr_to(t) : NULL;
        if(!strcmp(e->op,"*")) return t ? t->base : NULL;
        return t;
    case EX_BINARY: return parsed_expr_type(e->left);
    case EX_INDEX:
        t=parsed_expr_type(e->left); return t ? t->base : NULL;
    case EX_MEMBER: case EX_PTRMEMBER: {
        StructMember *m;
        t=parsed_expr_type(e->left);
        if(e->kind==EX_PTRMEMBER && t) t=t->base;
        m=find_struct_member(t,e->str); return m ? m->type : NULL;
    }
    case EX_CALL: return parsed_expr_type(e->left);
    default: return NULL;
    }
}

static void check_enum_conversion(Parser *p, CType *target, Expr *value)
{
    CType *source;
    size_t i;
    if(!target || !value) return;
    if(value->kind==EX_INITLIST && target->kind==TY_ARRAY) {
        for(i=0;i<value->nargs;i++) check_enum_conversion(p,target->base,value->args[i]);
        return;
    }
    if(target->kind!=TY_ENUM) return;
    if(value->kind==EX_INITLIST)
        perr(p,"enum object requires a scalar arithmetic initializer");
    source=parsed_expr_type(value);
    if(source && (source->kind==TY_PTR || source->kind==TY_ARRAY ||
                  source->kind==TY_STRUCT || source->kind==TY_VOID))
        perr(p,"enum object requires an arithmetic value, not a pointer or aggregate");
}

static void check_enum_lvalue(Parser *p, Expr *e)
{
    if(e && e->kind==EX_ENUM_CONST)
        perr(p,"enum constant '%s' is not a writable or addressable lvalue",e->str);
}

static bool parse_gnu_attributes(Parser *parser)
{
	bool packed = false;
	while(peq(parser, "__attribute__") || peq(parser, "__attribute")) {
		int depth = 0;
		parser->p++;
		pexpect(parser, "(");
		depth = 1;
		while(depth > 0) {
			if(ptok(parser)->kind == TK_EOF)
				perr(parser, "unterminated __attribute__");
			if(peq(parser, "packed") || peq(parser, "__packed__"))
				packed = true;
			if(peq(parser, "("))
				depth++;
			else if(peq(parser, ")"))
				depth--;
			parser->p++;
		}
	}
	return packed;
}

static void finish_struct_layout(CType *type, bool packed)
{
	long offset = 0;
	long maximum = 1;
	size_t index;
	type->packed = packed;
	for(index = 0; index < type->nmembers; index++) {
		long alignment = packed ? 1 : type_align(type->members[index].type);
		long size = type_size(type->members[index].type);
		if(alignment < 1)
			alignment = 1;
		offset = (offset + alignment - 1) / alignment * alignment;
		type->members[index].offset = offset;
		offset += size;
		if(alignment > maximum)
			maximum = alignment;
	}
	type->align = packed ? 1 : maximum;
	type->size = packed ? offset : (offset + maximum - 1) / maximum * maximum;
}

static CType *parse_type(Parser *parser)
{
	const char *character;
	CType *alias;
	bool is_unsigned = false;
	bool is_signed = false;
	while(peq(parser, "const") || peq(parser, "volatile") || peq(parser, "restrict"))
		parser->p++;
	if(paccept(parser, "enum")) return parse_enum(parser);
	if(paccept(parser, "struct")) {
		char *tag = NULL;
		CType *type;
		if(ptok(parser)->kind == TK_ID && !peq(parser, "{"))
			tag = pexpect_id(parser, false);
		if(!paccept(parser, "{")) {
			if(tag && find_enum_tag(parser,tag) && find_enum_tag(parser,tag)->depth>=struct_tag_depth(parser->prog,tag))
                perr(parser,"tag '%s' names an enum, not a struct",tag);
            type = find_struct_tag(parser->prog, tag ? tag : "");
			if(!type)
				perr(parser, "unknown struct '%s'", tag ? tag : "");
			return type;
		}
		if(tag && find_enum_tag(parser,tag) && find_enum_tag(parser,tag)->depth==parser->scope_depth)
			perr(parser,"struct tag conflicts with enum tag '%s'",tag);
		type = new_type(TY_STRUCT, NULL, 0, tag ? tag : "<anonymous>");
		add_struct_tag(parser, tag, type);
		while(!paccept(parser, "}")) {
			CType *member_base = parse_type(parser);
			Declarator member = parse_declarator(parser, member_base, false);
			if(member.function)
				perr(parser, "struct member cannot be a function");
			pexpect(parser, ";");
			ARR_GROW(type->members, type->nmembers, type->capmembers, StructMember);
			type->members[type->nmembers].name = member.name;
			type->members[type->nmembers].type = member.type;
			type->members[type->nmembers].offset = 0;
			type->nmembers++;
		}
		finish_struct_layout(type, parse_gnu_attributes(parser));
		return type;
	}
	if(paccept(parser, "unsigned"))
		is_unsigned = true;
	else if(paccept(parser, "signed"))
		is_signed = true;
	if(paccept(parser, "long")) {
		bool second_long = paccept(parser, "long");
		if(!second_long && paccept(parser, "double")) {
			if(is_unsigned || is_signed)
				perr(parser, "invalid signedness for long double");
			return &T_LDOUBLE;
		}
		paccept(parser, "int");
		if(is_unsigned)
			return second_long ? &T_U64 : &T_ULONG;
		return second_long ? &T_LLONG : &T_LONG;
	}
	if(paccept(parser, "short")) {
		if(paccept(parser, "int"))
			return is_unsigned ? &T_U16 : &T_SHORT;
		return is_unsigned ? &T_U16 : &T_SHORT;
	}
	if(paccept(parser, "string")) {
		if(is_unsigned || is_signed)
			perr(parser, "invalid signedness for string");
		return ptr_to(&T_CHAR);
	}
	if(paccept(parser, "char"))
		return is_unsigned ? &T_U8 : &T_CHAR;
	if(paccept(parser, "int"))
		return is_unsigned ? &T_U32 : &T_INT;
	if(is_unsigned)
		return &T_U32;
	if(is_signed)
		return &T_INT;
	if(paccept(parser, "float")) {
		if(is_unsigned || is_signed)
			perr(parser, "invalid signedness for float");
		return &T_FLOAT;
	}
	if(paccept(parser, "double")) {
		if(is_unsigned || is_signed)
			perr(parser, "invalid signedness for double");
		return &T_DOUBLE;
	}
	if(is_unsigned || is_signed)
		perr(parser, "expected integer type");
	character = ptok(parser)->v;
	alias = find_alias(parser->prog, character);
	if(alias) {
		parser->p++;
		return alias;
	}
	if(!strcmp(character, "void")) {
		parser->p++;
		return &T_VOID;
	}
	if(!strcmp(character, "_Bool") || !strcmp(character, "bool")) {
		parser->p++;
		return &T_BOOL;
	}
	if(!strcmp(character, "int8_t") || !strcmp(character, "int_least8_t") ||
	   !strcmp(character, "int_fast8_t")) {
		parser->p++;
		return &T_CHAR;
	}
	if(!strcmp(character, "uint8_t") || !strcmp(character, "uint_least8_t") ||
	   !strcmp(character, "uint_fast8_t")) {
		parser->p++;
		return &T_U8;
	}
	if(!strcmp(character, "int16_t") || !strcmp(character, "int_least16_t") ||
	   !strcmp(character, "int_fast16_t")) {
		parser->p++;
		return &T_SHORT;
	}
	if(!strcmp(character, "uint16_t") || !strcmp(character, "uint_least16_t") ||
	   !strcmp(character, "uint_fast16_t")) {
		parser->p++;
		return &T_U16;
	}
	if(!strcmp(character, "int32_t") || !strcmp(character, "int_least32_t") ||
	   !strcmp(character, "int_fast32_t")) {
		parser->p++;
		return &T_INT;
	}
	if(!strcmp(character, "uint32_t") || !strcmp(character, "uint_least32_t") ||
	   !strcmp(character, "uint_fast32_t")) {
		parser->p++;
		return &T_U32;
	}
	if(!strcmp(character, "int64_t") || !strcmp(character, "int_least64_t") ||
	   !strcmp(character, "int_fast64_t")) {
		parser->p++;
		return &T_I64;
	}
	if(!strcmp(character, "uint64_t") || !strcmp(character, "uint_least64_t") ||
	   !strcmp(character, "uint_fast64_t")) {
		parser->p++;
		return &T_U64;
	}
	if(!strcmp(character, "size_t") || !strcmp(character, "uintptr_t")) {
		parser->p++;
		return type_get_target() == TARGET_I386 ? &T_U32 : &T_U64;
	}
	if(!strcmp(character, "ssize_t") || !strcmp(character, "ptrdiff_t") ||
	   !strcmp(character, "intptr_t")) {
		parser->p++;
		return type_get_target() == TARGET_I386 ? &T_INT : &T_I64;
	}
	if(!strcmp(character, "intmax_t")) {
		parser->p++;
		return &T_I64;
	}
	if(!strcmp(character, "uintmax_t")) {
		parser->p++;
		return &T_U64;
	}
	if(!strcmp(character, "FILE")) {
		parser->p++;
		return &T_FILE;
	}
	if(!strcmp(character, "Display")) {
		parser->p++;
		return &T_DISPLAY;
	}
	if(!strcmp(character, "Window")) {
		parser->p++;
		return type_get_target() == TARGET_I386 ? &T_U32 : &T_U64;
	}
	if(!strcmp(character, "GC")) {
		parser->p++;
		return ptr_to(&T_VOID);
	}
	if(!strcmp(character, "Font")) {
		parser->p++;
		return type_get_target() == TARGET_I386 ? &T_U32 : &T_U64;
	}
	if(!strcmp(character, "XEvent")) {
		parser->p++;
		return &T_XEVENT;
	}
	if(!strcmp(character, "va_list")) {
		parser->p++;
		return &T_VALIST;
	}
	perr(parser, "expected type");
	return NULL;
}

static Declarator parse_declarator(Parser *parser, CType *base, bool unnamed)
{
	Declarator d = {0};
	CType *type_1 = base;
	while(paccept(parser, "*"))
		type_1 = ptr_to(type_1);
	d.name = pexpect_id(parser, unnamed);
	d.type = type_1;
	if(paccept(parser, "(")) {
		d.function = true;
		if(paccept(parser, ")"))
			return d;
		if(peq(parser, "void") &&
		   !strcmp(parser->ts->a[parser->p + 1].v, ")"))
		{
			parser->p += 2;
			return d;
		}
		for(;;) {
			CType *parsed_type;
			char *parameter_name;
			if(paccept(parser, "...")) {
				d.variadic = true;
				pexpect(parser, ")");
				break;
			}
			parsed_type = parse_type(parser);
			while(paccept(parser, "*"))
				parsed_type = ptr_to(parsed_type);
			parameter_name = pexpect_id(parser, true);
			if(!*parameter_name) {
				char temporary_buffer[32];
				snprintf(temporary_buffer, sizeof(temporary_buffer), "__arg%zu", d.nparams);
				parameter_name = xstrdup(temporary_buffer);
			}
			if(paccept(parser, "[")) {
				if(ptok(parser)->kind == TK_NUM)
					parser->p++;
				pexpect(parser, "]");
				parsed_type = ptr_to(parsed_type);
			}
			ARR_GROW(d.params, d.nparams, d.capparams, Param);
			d.params[d.nparams].name = parameter_name;
			d.params[d.nparams].type = parsed_type;
			d.nparams++;
			if(paccept(parser, ")"))
				break;
			pexpect(parser, ",");
		}
		return d;
	}
	while(paccept(parser, "[")) {
		Expr *bound;
		long count;
		if(paccept(parser, "]")) {
			type_1 = array_of(type_1, 0);
			d.type = type_1;
			continue;
		}
		bound = parse_expr(parser, 1);
		pexpect(parser, "]");
		if(eval_const_expr(bound, &count)) {
			if(count < 0)
				perr(parser, "array length cannot be negative");
			type_1 = array_of(type_1, count);
		} else if(bound->kind == EX_ID)
		{
			type_1 = vla_of(type_1, bound->str);
		} else {
			perr(parser, "array length must be a constant expression or identifier");
		}
		d.type = type_1;
	}
	return d;
}

static char decode_escape(const char **pointer_pointer)
{
	const char *position = *pointer_pointer;
	char character = *position++;
	if(character != '\\') {
		*pointer_pointer = position;
		return character;
	}
	character = *position++;
	switch(character) {
	case 'a':
		character = '\a';
		break;
	case 'b':
		character = '\b';
		break;
	case 'f':
		character = '\f';
		break;
	case 'n':
		character = '\n';
		break;
	case 'r':
		character = '\r';
		break;
	case 't':
		character = '\t';
		break;
	case 'v':
		character = '\v';
		break;
	case '0':
		character = '\0';
		break;
	case '\\':
		character = '\\';
		break;
	case '\'':
		character = '\'';
		break;
	case '"':
		character = '"';
		break;
	default:
		break;
	}
	*pointer_pointer = position;
	return character;
}

static char *decode_string(const char *raw)
{
	size_t length = strlen(raw), index = 1;
	Buf buffer = {0};
	while(index + 1 < length) {
		const char *position = raw + index;
		char character = decode_escape(&position);
		bputn(&buffer, &character, 1);
		index = (size_t)(position - raw);
	}
	if(!buffer.s)
		buffer.s = xstrdup("");
	return buffer.s;
}


static void parse_match_pattern(Parser *parser, MatchArm *arm)
{
	Expr *low;
	Expr *high = NULL;
	bool range = false;
	if(peq(parser, "_") || peq(parser, "default")) {
		parser->p++;
		if(arm->is_default)
			perr(parser, "duplicate default match pattern");
		arm->is_default = true;
	} else {
		low = parse_expr(parser, 1);
		if(paccept(parser, "...")) {
			high = parse_expr(parser, 1);
			range = true;
		}
		ARR_GROW(arm->patterns, arm->npatterns, arm->cappatterns, Expr *);
		arm->patterns[arm->npatterns] = low;
		ARR_GROW(arm->pattern_highs, arm->npatterns, arm->cappattern_highs, Expr *);
		ARR_GROW(arm->pattern_ranges, arm->npatterns, arm->cappattern_ranges, bool);
		arm->pattern_highs[arm->npatterns] = high;
		arm->pattern_ranges[arm->npatterns] = range;
		arm->npatterns++;
	}
	while(paccept(parser, ",")) {
		if(peq(parser, "_") || peq(parser, "default")) {
			parser->p++;
			if(arm->npatterns || arm->is_default)
				perr(parser, "wildcard must be the only match pattern");
			arm->is_default = true;
			break;
		}
		low = parse_expr(parser, 1);
		high = NULL;
		range = false;
		if(paccept(parser, "...")) {
			high = parse_expr(parser, 1);
			range = true;
		}
		ARR_GROW(arm->patterns, arm->npatterns, arm->cappatterns, Expr *);
		arm->patterns[arm->npatterns] = low;
		ARR_GROW(arm->pattern_highs, arm->npatterns, arm->cappattern_highs, Expr *);
		ARR_GROW(arm->pattern_ranges, arm->npatterns, arm->cappattern_ranges, bool);
		arm->pattern_highs[arm->npatterns] = high;
		arm->pattern_ranges[arm->npatterns] = range;
		arm->npatterns++;
	}
	if(arm->is_default && arm->npatterns)
		perr(parser, "wildcard cannot be combined with other match patterns");
	if(paccept(parser, "if"))
		arm->guard = parse_expr(parser, 1);
	pexpect(parser, ":");
}

static Expr *parse_match_expr(Parser *parser)
{
	Expr *expression = new_expr(EX_MATCH);
	expression->left = parse_expr(parser, 1);
	pexpect(parser, "{");
	while(!paccept(parser, "}")) {
		MatchArm arm = {0};
		parse_match_pattern(parser, &arm);
		arm.expr = parse_expr(parser, 1);
		if(!paccept(parser, ";"))
			perr(parser, "match expression arm requires ';'");
		ARR_GROW(expression->arms, expression->narms, expression->caparms, MatchArm);
		expression->arms[expression->narms++] = arm;
	}
	return expression;
}

static Expr *parse_primary(Parser *parser)
{
	Token *token = ptok(parser);
	Expr *expression;
	if(paccept(parser, "(")) {
		expression = parse_expr(parser, 1);
		pexpect(parser, ")");
		return expression;
	}
	if(token->kind == TK_NUM) {
		bool floating = strchr(token->v, '.') != NULL;
		const char *number = token->v;
		if(!floating && !(number[0] == '0' && (number[1] == 'x' || number[1] == 'X')) &&
		   (strchr(number, 'e') || strchr(number, 'E')))
			floating = true;
		if(floating) {
			expression = new_expr(EX_NUM);
			expression->fnum = strtod(token->v, NULL);
			expression->type = &T_DOUBLE;
			parser->p++;
			return expression;
		}
		char *cursor_1 = xstrdup(token->v), *x_value = cursor_1;
		char *suffix = NULL;
		unsigned long long value;
		bool wide = false;
		bool uns = false;
		while(*x_value) {
			if(strchr("uUlL", *x_value)) {
				suffix = x_value;
				*x_value = 0;
				break;
			}
			x_value++;
		}
		if(suffix) {
			const char *result = token->v + (suffix - cursor_1);
			while(*result) {
				if(*result == 'u' || *result == 'U')
					uns = true;
				if((*result == 'l' || *result == 'L') &&
				   (result[1] == 'l' || result[1] == 'L'))
					wide = true;
				result++;
			}
		}
		value = strtoull(cursor_1, NULL, 0);
		free(cursor_1);
		parser->p++;
		expression = new_expr(EX_NUM);
		expression->num = value;
		expression->type = (wide || value > 0xffffffffULL) ? &T_U64 : (uns ? &T_U32 : &T_INT);
		return expression;
	}
	if(token->kind == TK_STR) {
		Buf joined = {0};
		while(ptok(parser)->kind == TK_STR) {
			char *part = decode_string(ptok(parser)->v);
			bputs(&joined, part);
			free(part);
			parser->p++;
		}
		expression = new_expr(EX_STR);
		expression->str = joined.s ? joined.s : xstrdup("");
		expression->type = ptr_to(&T_CHAR);
		return expression;
	}
	if(token->kind == TK_CHAR) {
		const char *cursor_1 = token->v + 1;
		char character = decode_escape(&cursor_1);
		parser->p++;
		expression = new_expr(EX_NUM);
		expression->num = (unsigned char)character;
		expression->type = &T_INT;
		return expression;
	}
	if(paccept(parser, "match")) {
		expression = parse_match_expr(parser);
		return expression;
	}
	if(paccept(parser, "typeof")) {
		expression = new_expr(EX_TYPEOF);
		pexpect(parser, "(");
		if(type_start(parser))
			expression->sizeof_type = parse_type(parser);
		else
			expression->left = parse_expr(parser, 1);
		pexpect(parser, ")");
		return expression;
	}
	if(type_start(parser)) {
		expression = new_expr(EX_TYPE);
		expression->type = parse_type(parser);
		while(paccept(parser, "*"))
			expression->type = ptr_to(expression->type);
		return expression;
	}
	if(token->kind == TK_ID) {
        NameBinding *binding=find_binding(parser,token->v);
        size_t i;
        if(!binding) for(i=0;i<parser->prog->nbindings;i++) {
            NameBinding *old=&parser->prog->bindings[i];
            if(old->is_enum_constant && !strcmp(old->name,token->v))
                perr(parser,"enum constant '%s' is outside its declaration scope",token->v);
        }
        parser->p++;
        expression=new_expr(binding && binding->is_enum_constant ? EX_ENUM_CONST : EX_ID);
        expression->str=token->v;
        if(binding) {
            expression->type=binding->type;
            expression->num=(unsigned long long)binding->value;
        }
        return expression;
    }
	perr(parser, "expected expression");
	return NULL;
}

static Expr *parse_postfix(Parser *parser)
{
	Expr *expression = parse_primary(parser);
	for(;;) {
		if(paccept(parser, "(")) {
			Expr *call_expression = new_expr(EX_CALL);
			call_expression->left = expression;
			if(!paccept(parser, ")"))
				for(;;) {
					Expr *expression_1 = parse_expr(parser, 1);
					ARR_GROW(call_expression->args, call_expression->nargs, call_expression->capargs, Expr *);
					call_expression->args[call_expression->nargs++] = expression_1;
					if(paccept(parser, ")"))
						break;
					pexpect(parser, ",");
				}
			            if(call_expression->left->kind==EX_ID) {
                Decl *function=NULL;
                size_t i;
                const char *name=call_expression->left->str;
                if(parser->current_function && !strcmp(parser->current_function->name,name))
                    function=parser->current_function;
                for(i=0;!function && i<parser->prog->n;i++) {
                    Decl *candidate=parser->prog->a[i];
                    if((candidate->prototype || candidate->body) && !strcmp(candidate->name,name)) function=candidate;
                }
                if(function) for(i=0;i<function->nparams && i<call_expression->nargs;i++)
                    check_enum_conversion(parser,function->params[i].type,call_expression->args[i]);
            }
            expression = call_expression;
			continue;
		}
		if(paccept(parser, "[")) {
			Expr *expression_2 = new_expr(EX_INDEX);
			expression_2->left = expression;
			expression_2->right = parse_expr(parser, 1);
			pexpect(parser, "]");
			expression = expression_2;
			continue;
		}
		if(paccept(parser, ".")) {
			Expr *expression_2 = new_expr(EX_MEMBER);
			expression_2->left = expression;
			expression_2->str = pexpect_id(parser, false);
			expression = expression_2;
			continue;
		}
		if(paccept(parser, "->")) {
			Expr *expression_2 = new_expr(EX_PTRMEMBER);
			expression_2->left = expression;
			expression_2->str = pexpect_id(parser, false);
			expression = expression_2;
			continue;
		}
		if(paccept(parser, "++")) {
			Expr *expression_2 = new_expr(EX_UNARY);
			check_enum_lvalue(parser,expression);
			expression_2->op = "post++";
			expression_2->left = expression;
			expression = expression_2;
			continue;
		}
		if(paccept(parser, "--")) {
			Expr *expression_2 = new_expr(EX_UNARY);
			check_enum_lvalue(parser,expression);
			expression_2->op = "post--";
			expression_2->left = expression;
			expression = expression_2;
			continue;
		}
		break;
	}
	return expression;
}

static Expr *parse_unary(Parser *parser)
{
	if(peq(parser, "(")) {
		size_t save = parser->p;
		CType *cast_type;
		Expr *expression_1;
		parser->p++;
		if(type_start(parser)) {
			cast_type = parse_type(parser);
			while(paccept(parser, "*"))
				cast_type = ptr_to(cast_type);
			if(paccept(parser, ")")) {
				expression_1 = new_expr(EX_UNARY);
				expression_1->op = "cast";
				expression_1->type = cast_type;
				expression_1->left = parse_unary(parser);
				return expression_1;
			}
		}
		parser->p = save;
	}
	if(peq(parser, "++") || peq(parser, "--")) {
		Expr *expression_1 = new_expr(EX_UNARY);
		expression_1->op = ptok(parser)->v;
		parser->p++;
		expression_1->left = parse_unary(parser);
        if(!strcmp(expression_1->op,"++") || !strcmp(expression_1->op,"--") || !strcmp(expression_1->op,"&"))
            check_enum_lvalue(parser,expression_1->left);
        if(strcmp(expression_1->op,"++") && strcmp(expression_1->op,"--") &&
           strcmp(expression_1->op,"&") && strcmp(expression_1->op,"*") &&
           parsed_expr_type(expression_1->left) && parsed_expr_type(expression_1->left)->kind==TY_ENUM)
            expression_1->type=&T_INT;
		return expression_1;
	}
	if(peq(parser, "!") || peq(parser, "~") || peq(parser, "-") || peq(parser, "+") || peq(parser, "&") || peq(parser, "*")) {
		Expr *expression_1 = new_expr(EX_UNARY);
		expression_1->op = ptok(parser)->v;
		parser->p++;
		expression_1->left = parse_unary(parser);
        if(!strcmp(expression_1->op,"++") || !strcmp(expression_1->op,"--") || !strcmp(expression_1->op,"&"))
            check_enum_lvalue(parser,expression_1->left);
        if(strcmp(expression_1->op,"++") && strcmp(expression_1->op,"--") &&
           strcmp(expression_1->op,"&") && strcmp(expression_1->op,"*") &&
           parsed_expr_type(expression_1->left) && parsed_expr_type(expression_1->left)->kind==TY_ENUM)
            expression_1->type=&T_INT;
		return expression_1;
	}
	if(paccept(parser, "sizeof")) {
		Expr *expression_1 = new_expr(EX_SIZEOF);
		pexpect(parser, "(");
		if(type_start(parser)) {
			expression_1->sizeof_type = parse_type(parser);
			while(paccept(parser, "*"))
				expression_1->sizeof_type = ptr_to(expression_1->sizeof_type);
		} else {
			expression_1->left = parse_expr(parser, 1);
		}
		pexpect(parser, ")");
		return expression_1;
	}
	return parse_postfix(parser);
}

static int prec(const char *string)
{
	if(!strcmp(string, "=") || !strcmp(string, "+=") || !strcmp(string, "-=") ||
	   !strcmp(string, "*=") || !strcmp(string, "/=") || !strcmp(string, "%=") ||
	   !strcmp(string, "&=") || !strcmp(string, "|=") || !strcmp(string, "^=") ||
	   !strcmp(string, "<<=") || !strcmp(string, ">>="))
		return 1;
	if(!strcmp(string, "||"))
		return 2;
	if(!strcmp(string, "&&"))
		return 3;
	if(!strcmp(string, "|"))
		return 4;
	if(!strcmp(string, "^"))
		return 5;
	if(!strcmp(string, "&"))
		return 6;
	if(!strcmp(string, "==") || !strcmp(string, "!="))
		return 7;
	if(!strcmp(string, "<") || !strcmp(string, "<=") || !strcmp(string, ">") || !strcmp(string, ">="))
		return 8;
	if(!strcmp(string, "<<") || !strcmp(string, ">>"))
		return 9;
	if(!strcmp(string, "+") || !strcmp(string, "-"))
		return 10;
	if(!strcmp(string, "*") || !strcmp(string, "/") || !strcmp(string, "%"))
		return 11;
	return 0;
}

static Expr *parse_expr(Parser *parser, int minprec)
{
	Expr *lhs = parse_unary(parser);
	for(;;) {
		int precedence = prec(ptok(parser)->v);
		char *operator;
		Expr *rhs, *expression;
		bool right;
		if(precedence < minprec)
			break;
		operator = ptok(parser)->v;
		parser->p++;
		right = (!strcmp(operator, "=") || !strcmp(operator, "+=") || !strcmp(operator, "-=") ||
			 !strcmp(operator, "*=") || !strcmp(operator, "/=") || !strcmp(operator, "%=") ||
			 !strcmp(operator, "&=") || !strcmp(operator, "|=") || !strcmp(operator, "^=") ||
			 !strcmp(operator, "<<=") || !strcmp(operator, ">>="));
		rhs = parse_expr(parser, right ? precedence : precedence + 1);
        if(right) { check_enum_lvalue(parser,lhs); check_enum_conversion(parser,parsed_expr_type(lhs),rhs); }
		expression = new_expr(EX_BINARY);
		expression->op = operator;
		expression->left = lhs;
		expression->right = rhs;
        if(!right) {
            CType *lt=parsed_expr_type(lhs), *rt=parsed_expr_type(rhs);
            if((lt && lt->kind==TY_ENUM) || (rt && rt->kind==TY_ENUM)) {
                if(precedence==2 || precedence==3 || precedence==7 || precedence==8)
                    expression->type=&T_INT;
                else if((lt && lt->kind==TY_DOUBLE) || (rt && rt->kind==TY_DOUBLE))
                    expression->type=&T_DOUBLE;
                else if((lt && (lt->kind==TY_PTR || lt->kind==TY_ARRAY)) ||
                        (rt && (rt->kind==TY_PTR || rt->kind==TY_ARRAY))) {
                    /* Leave pointer arithmetic typing to the existing backend. */
                } else if(lt && lt->kind!=TY_ENUM && lt->kind!=TY_BOOL && lt->kind!=TY_CHAR && lt->kind!=TY_SHORT)
                    expression->type=lt;
                else if(rt && rt->kind!=TY_ENUM && rt->kind!=TY_BOOL && rt->kind!=TY_CHAR && rt->kind!=TY_SHORT)
                    expression->type=rt;
                else expression->type=&T_INT;
            }
        }
		lhs = expression;
	}
	return lhs;
}

static Expr *parse_initializer(Parser *parser)
{
	Expr *expression;
	if(!paccept(parser, "{"))
		return parse_expr(parser, 1);
	expression = new_expr(EX_INITLIST);
	if(paccept(parser, "}"))
		return expression;
	for(;;) {
		Expr *item = parse_initializer(parser);
		ARR_GROW(expression->args, expression->nargs, expression->capargs, Expr *);
		expression->args[expression->nargs++] = item;
		if(paccept(parser, "}"))
			break;
		pexpect(parser, ",");
		if(paccept(parser, "}"))
			break;
	}
	return expression;
}


bool eval_const_expr(Expr *expression, long *value)
{
    long a,b;
    const char *op;
    if(!expression) return false;
    if(expression->kind==EX_ENUM_CONST || expression->kind==EX_NUM) {
        if(expression->type && (expression->type->kind==TY_DOUBLE || expression->type->kind==TY_FLOAT || expression->type->kind==TY_LDOUBLE)) return false;
        if(expression->kind==EX_NUM && expression->num>LONG_MAX) return false;
        *value=(long)expression->num; return true;
    }
    if(expression->kind==EX_SIZEOF) {
        CType *type=expression->sizeof_type ? expression->sizeof_type : parsed_expr_type(expression->left);
        if(!type || is_vla(type)) return false;
        *value=type_size(type); return true;
    }
    if(expression->kind==EX_ID && !strcmp(expression->str,"NULL")) { *value=0; return true; }
    if(expression->kind==EX_UNARY) {
        if(!eval_const_expr(expression->left,&a)) return false;
        op=expression->op;
        if(!strcmp(op,"+")) *value=a;
        else if(!strcmp(op,"cast")) {
            CType *t=expression->type;
            if(!t || t->kind==TY_PTR || t->kind==TY_DOUBLE || t->kind==TY_FLOAT || t->kind==TY_LDOUBLE || t->kind==TY_VOID || t->kind==TY_STRUCT) return false;
            switch(t->kind) {
            case TY_ENUM: case TY_INT: *value=(int32_t)a; break;
            case TY_U32: *value=(uint32_t)a; break;
            case TY_CHAR: *value=(int8_t)a; break;
            case TY_U8: *value=(uint8_t)a; break;
            case TY_SHORT: *value=(int16_t)a; break;
            case TY_U16: *value=(uint16_t)a; break;
            case TY_BOOL: *value=!!a; break;
            default: *value=a; break;
            }
        } else if(!strcmp(op,"-")) { if(a==LONG_MIN) return false; *value=-a; }
        else if(!strcmp(op,"~")) *value=~a;
        else if(!strcmp(op,"!")) *value=!a;
        else return false;
        return true;
    }
    if(expression->kind!=EX_BINARY || !eval_const_expr(expression->left,&a)) return false;
    op=expression->op;
    if(!strcmp(op,"&&") && !a) { *value=0; return true; }
    if(!strcmp(op,"||") && a) { *value=1; return true; }
    if(!eval_const_expr(expression->right,&b)) return false;
    if(!strcmp(op,"+")) return !__builtin_add_overflow(a,b,value);
    if(!strcmp(op,"-")) return !__builtin_sub_overflow(a,b,value);
    if(!strcmp(op,"*")) return !__builtin_mul_overflow(a,b,value);
    if(!strcmp(op,"/") || !strcmp(op,"%")) {
        if(!b || (a==LONG_MIN && b==-1)) return false;
        *value=!strcmp(op,"/") ? a/b : a%b;
    } else if(!strcmp(op,"<<")) {
        if(b<0 || b>=(long)(sizeof(long)*CHAR_BIT) || a<0 || a>(LONG_MAX>>b)) return false;
        *value=a<<b;
    } else if(!strcmp(op,">>")) {
        if(b<0 || b>=(long)(sizeof(long)*CHAR_BIT)) return false;
        *value=a>>b;
    } else if(!strcmp(op,"&")) *value=a&b;
    else if(!strcmp(op,"|")) *value=a|b;
    else if(!strcmp(op,"^")) *value=a^b;
    else if(!strcmp(op,"==")) *value=a==b;
    else if(!strcmp(op,"!=")) *value=a!=b;
    else if(!strcmp(op,"<")) *value=a<b;
    else if(!strcmp(op,"<=")) *value=a<=b;
    else if(!strcmp(op,">")) *value=a>b;
    else if(!strcmp(op,">=")) *value=a>=b;
    else if(!strcmp(op,"&&")) *value=!!a && !!b;
    else if(!strcmp(op,"||")) *value=!!a || !!b;
    else return false;
    return true;
}

static void infer_array_bound(Parser *parser, CType *type, Expr *initializer)
{
	if(!type || type->kind != TY_ARRAY || type->count != 0)
		return;
	if(!initializer)
		return;
	if(initializer->kind == EX_INITLIST)
		type->count = (long)initializer->nargs;
	else if(initializer->kind == EX_STR &&
		(type->base->kind == TY_CHAR || type->base->kind == TY_U8))
		type->count = (long)strlen(initializer->str) + 1;
	else
		perr(parser, "array with omitted length requires an initializer");
}
static Stmt *parse_stmt(Parser *p);

static Stmt *parse_block(Parser *parser)
{
	Stmt *statement = new_stmt(ST_BLOCK);
	pexpect(parser, "{");
    parser->scope_depth++;
    if(parser->pending_params) {
        size_t i;
        for(i=0;i<parser->npending_params;i++) {
            Param *param=&parser->pending_params[i];
            bind_name(parser,param->name,param->type,false,0,false);
        }
        parser->pending_params=NULL;
        parser->npending_params=0;
    }
	parser->recovery_brace_depth++;
	while(!paccept(parser, "}")) {
		Stmt *statement_1;
		if(paccept(parser, "var")) {
			Decl *declaration = new_decl();
			declaration->name = pexpect_id(parser, false);
			if(!paccept(parser, "="))
				perr(parser, "var declaration requires an initializer");
			declaration->init = parse_initializer(parser);
			declaration->is_var = true;
            bind_name(parser,declaration->name,parsed_expr_type(declaration->init),false,0,false);
			statement_1 = new_stmt(ST_DECL);
			statement_1->decl = declaration;
			ARR_GROW(statement->children, statement->nchildren, statement->capchildren, Stmt *);
			statement->children[statement->nchildren++] = statement_1;
			pexpect(parser, ";");
			continue;
		}
		if(type_start(parser)) {
			CType *base_type;
            parser->enum_definition=false;
            base_type=parse_type(parser);
            if(base_type->kind==TY_ENUM && parser->enum_definition &&
               (paccept(parser,";") || type_start(parser) || reserved_enum_name(ptok(parser)->v) || peq(parser,"}"))) continue;
            for(;;) {
				Declarator q = parse_declarator(parser, base_type, false);
				Decl *declaration = new_decl();
				if(q.function)
					perr(parser, "nested function unsupported");
				declaration->name = q.name;
				declaration->type = q.type;
                bind_name(parser,q.name,q.type,false,0,false);
				if(paccept(parser, "="))
					declaration->init = parse_initializer(parser);
				check_enum_conversion(parser,declaration->type,declaration->init);
                infer_array_bound(parser, declaration->type, declaration->init);
				statement_1 = new_stmt(ST_DECL);
				statement_1->decl = declaration;
				ARR_GROW(statement->children, statement->nchildren, statement->capchildren, Stmt *);
				statement->children[statement->nchildren++] = statement_1;
				if(!paccept(parser, ","))
					break;
			}
			pexpect(parser, ";");
			continue;
		}
		statement_1 = parse_stmt(parser);
		ARR_GROW(statement->children, statement->nchildren, statement->capchildren, Stmt *);
		statement->children[statement->nchildren++] = statement_1;
	}
	parser->recovery_brace_depth--;
    leave_scope(parser);
	return statement;
}

static Stmt *parse_stmt(Parser *parser)
{
	Stmt *statement;
	if(peq(parser, "{"))
		return parse_block(parser);
	if(paccept(parser, "if")) {
		statement = new_stmt(ST_IF);
		pexpect(parser, "(");
		statement->cond = parse_expr(parser, 1);
		pexpect(parser, ")");
		statement->yes = parse_stmt(parser);
		if(paccept(parser, "else"))
			statement->no = parse_stmt(parser);
		return statement;
	}
	if(paccept(parser, "while")) {
		statement = new_stmt(ST_WHILE);
		pexpect(parser, "(");
		statement->cond = parse_expr(parser, 1);
		pexpect(parser, ")");
		statement->body = parse_stmt(parser);
		return statement;
	}
	if(paccept(parser, "for")) {
        parser->scope_depth++;
		statement = new_stmt(ST_FOR);
		pexpect(parser, "(");
		if(!paccept(parser, ";")) {
			if(paccept(parser, "var")) {
				Decl *declaration = new_decl();
				declaration->name = pexpect_id(parser, false);
				if(!paccept(parser, "="))
					perr(parser, "var declaration requires an initializer");
				declaration->init = parse_initializer(parser);
				declaration->is_var = true;
            bind_name(parser,declaration->name,parsed_expr_type(declaration->init),false,0,false);
				pexpect(parser, ";");
				statement->init = new_stmt(ST_DECL);
				statement->init->decl = declaration;
			} else if(type_start(parser)) {
				CType *base_type = parse_type(parser);
				Declarator q = parse_declarator(parser, base_type, false);
				Decl *declaration = new_decl();
				if(q.function)
					perr(parser, "function declaration in for initializer unsupported");
				declaration->name = q.name;
				declaration->type = q.type;
                bind_name(parser,q.name,q.type,false,0,false);
				if(paccept(parser, "="))
					declaration->init = parse_initializer(parser);
				check_enum_conversion(parser,declaration->type,declaration->init);
                infer_array_bound(parser, declaration->type, declaration->init);
				pexpect(parser, ";");
				statement->init = new_stmt(ST_DECL);
				statement->init->decl = declaration;
			} else {
				statement->init = new_stmt(ST_EXPR);
				statement->init->expr = parse_expr(parser, 1);
				pexpect(parser, ";");
			}
		}
		if(!paccept(parser, ";")) {
			statement->cond = parse_expr(parser, 1);
			pexpect(parser, ";");
		}
		if(!paccept(parser, ")")) {
			statement->post = parse_expr(parser, 1);
			pexpect(parser, ")");
		}
		statement->body = parse_stmt(parser);
        leave_scope(parser);
		return statement;
	}
	if(paccept(parser, "match")) {
		statement = new_stmt(ST_MATCH);
		statement->expr = parse_expr(parser, 1);
		pexpect(parser, "{");
		while(!paccept(parser, "}")) {
			MatchArm arm = {0};
			parse_match_pattern(parser, &arm);
			arm.stmt = parse_stmt(parser);
			ARR_GROW(statement->arms, statement->narms, statement->caparms, MatchArm);
			statement->arms[statement->narms++] = arm;
		}
		return statement;
	}
	if(paccept(parser, "switch")) {
		statement = new_stmt(ST_SWITCH);
		pexpect(parser, "(");
		statement->cond = parse_expr(parser, 1);
		pexpect(parser, ")");
		statement->body = parse_stmt(parser);
		return statement;
	}
	if(paccept(parser, "case")) {
		statement = new_stmt(ST_CASE);
		statement->expr = parse_expr(parser, 1);
		pexpect(parser, ":");
		return statement;
	}
	if(paccept(parser, "default")) {
		statement = new_stmt(ST_DEFAULT);
		pexpect(parser, ":");
		return statement;
	}
	if(paccept(parser, "return")) {
		statement = new_stmt(ST_RETURN);
		if(!paccept(parser, ";")) {
			statement->expr = parse_expr(parser, 1);
            if(parser->current_function)
                check_enum_conversion(parser,parser->current_function->type,statement->expr);
			pexpect(parser, ";");
		}
		return statement;
	}
	if(paccept(parser, "break")) {
		pexpect(parser, ";");
		return new_stmt(ST_BREAK);
	}
	if(paccept(parser, "continue")) {
		pexpect(parser, ";");
		return new_stmt(ST_CONTINUE);
	}
	if(paccept(parser, "asm") || paccept(parser, "__asm__")) {
		Buf text = {0};
		statement = new_stmt(ST_ASM);
		paccept(parser, "volatile");
		paccept(parser, "__volatile__");
		pexpect(parser, "(");
		if(ptok(parser)->kind != TK_STR)
			perr(parser, "inline asm requires a string literal");
		while(ptok(parser)->kind == TK_STR) {
			char *part = decode_string(ptok(parser)->v);
			bputs(&text, part);
			free(part);
			parser->p++;
		}
		if(paccept(parser, ":")) {
			if(!peq(parser, ")") && !peq(parser, ":")) {
				for(;;) {
					char *constraint;
					Expr *output;
					if(ptok(parser)->kind != TK_STR)
						perr(parser, "asm output constraint must be a string");
					constraint = decode_string(ptok(parser)->v);
					parser->p++;
					pexpect(parser, "(");
					output = parse_expr(parser, 1);
					pexpect(parser, ")");
					ARR_GROW(statement->asm_constraints, statement->nasm_outputs, statement->capasm_constraints, char *);
					statement->asm_constraints[statement->nasm_outputs] = constraint;
					ARR_GROW(statement->asm_outputs, statement->nasm_outputs, statement->capasm_outputs, Expr *);
					statement->asm_outputs[statement->nasm_outputs] = output;
					statement->nasm_outputs++;
					if(!paccept(parser, ","))
						break;
				}
			}
			//Inputs and clobbers are parsed only when empty for now.
			if(paccept(parser, ":")) {
				if(!peq(parser, ")") && !peq(parser, ":"))
					perr(parser, "asm inputs are not supported yet");
				paccept(parser, ":");
			}
		}
		pexpect(parser, ")");
		pexpect(parser, ";");
		statement->asm_text = text.s ? text.s : xstrdup("");
		return statement;
	}
	if(paccept(parser, ";"))
		return new_stmt(ST_EMPTY);
	statement = new_stmt(ST_EXPR);
	statement->expr = parse_expr(parser, 1);
	pexpect(parser, ";");
	return statement;
}

void parse_program(Tokens *token_stream, Program *prog)
{
	Parser p = {0};
	p.ts = token_stream;
	p.p = 0;
	p.prog = prog;
	p.recovery_brace_depth = 0;
	while(ptok(&p)->kind != TK_EOF) {
		if(setjmp(p.error_jmp)) {
			sync_parser(&p);
            while(p.scope_depth>0) leave_scope(&p);
            p.pending_params=NULL; p.npending_params=0; p.current_function=NULL;
			continue;
		}
		bool is_typedef = false;
		bool is_extern = false;
		bool is_static = false;
		bool is_inline = false;
		CType *base_type;
		Declarator q;
		Decl *declaration;
		for(;;) {
			if(paccept(&p, "typedef")) {
				if(is_typedef)
					perr(&p, "duplicate typedef specifier");
				is_typedef = true;
				continue;
			}
			if(paccept(&p, "extern")) {
				if(is_extern)
					perr(&p, "duplicate extern specifier");
				is_extern = true;
				continue;
			}
			if(paccept(&p, "static")) {
				if(is_static)
					perr(&p, "duplicate static specifier");
				is_static = true;
				continue;
			}
			if(paccept(&p, "inline")) {
				if(is_inline)
					perr(&p, "duplicate inline specifier");
				is_inline = true;
				continue;
			}
			break;
		}
		if(is_extern && is_static)
			perr(&p, "declaration cannot be both extern and static");
		if(is_typedef && (is_extern || is_static || is_inline))
			perr(&p, "typedef cannot be combined with extern, static, or inline");
		p.enum_definition=false;
        base_type = parse_type(&p);
        if(base_type->kind==TY_ENUM && p.enum_definition && !is_typedef &&
           (ptok(&p)->kind==TK_EOF || type_start(&p) || reserved_enum_name(ptok(&p)->v))) continue;
		if(paccept(&p, ";")) {
			if(is_typedef || is_extern || is_static || is_inline)
				perr(&p, "declaration specifier requires a declarator");
			continue;
		}
		q = parse_declarator(&p, base_type, false);
		if(is_typedef) {
			if(q.function)
				perr(&p, "function typedefs are not supported yet");
			pexpect(&p, ";");
            NameBinding *old=find_binding(&p,q.name);
            if(old && old->is_enum_constant && old->depth==p.scope_depth)
                perr(&p,"typedef '%s' conflicts with enum constant",q.name);
			add_alias(&p, q.name, q.type);
			continue;
		}
		if(is_inline && !q.function)
			perr(&p, "inline can only be used on a function");
		declaration = new_decl();
		declaration->name = q.name;
		declaration->type = q.type;
		declaration->params = q.params;
		declaration->nparams = q.nparams;
		declaration->capparams = q.capparams;
		declaration->variadic = q.variadic;
		declaration->is_extern = is_extern;
		declaration->is_static = is_static;
		declaration->is_inline = is_inline;
        bind_name(&p,q.name,q.type,false,0,q.function);
		if(q.function) {
			if(paccept(&p, ";"))
				declaration->prototype = true;
			else {
				if(is_extern)
					perr(&p, "extern function cannot have a body");
				p.pending_params=q.params; p.npending_params=q.nparams;
                p.current_function=declaration;
                declaration->body = parse_block(&p);
                p.current_function=NULL;
			}
		} else {
			if(paccept(&p, "=")) {
				if(is_extern)
					perr(&p, "extern object cannot have an initializer");
				declaration->init = parse_initializer(&p);
			}
			check_enum_conversion(&p,declaration->type,declaration->init);
            if(declaration->type->kind==TY_ENUM && declaration->init) {
                long value;
                if(!eval_const_expr(declaration->init,&value))
                    perr(&p,"global enum initializer must be an integer constant expression");
            }
            infer_array_bound(&p, declaration->type, declaration->init);
			pexpect(&p, ";");
		}
		ARR_GROW(prog->a, prog->n, prog->cap, Decl *);
		prog->a[prog->n++] = declaration;
	}
}
