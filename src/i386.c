#include "i386.h"

//i386 backend
typedef struct
{
	char *name;
	CType *type;
	bool function;
	bool defined;
	bool local;
	uint16_t section;
	uint32_t value;
	uint32_t size;
	uint32_t index;
} I386ObjSymbol;
typedef struct
{
	uint32_t offset;
	uint32_t type;
	char *symbol;
	uint16_t section_symbol;
	uint16_t target_section;
} I386Relocation;
typedef struct
{
	char *name;
	uint32_t offset;
	bool defined;
} I386Label;
typedef struct
{
	char *name;
	uint32_t offset;
} I386JumpFixup;
typedef struct
{
	char *name;
	CType *type;
	long offset;
} I386Local;
typedef struct
{
	char *value;
	uint32_t offset;
} I386String;
typedef struct
{
	Program *program;
	Buf text;
	Buf rodata;
	Buf data;
	uint32_t bss_size;
	I386ObjSymbol *symbols;
	size_t nsymbols;
	size_t capsymbols;
	I386Relocation *relocations;
	size_t nrelocations;
	size_t caprelocations;
	I386Label *labels;
	size_t nlabels;
	size_t caplabels;
	I386JumpFixup *jump_fixups;
	size_t njump_fixups;
	size_t capjump_fixups;
	I386Local *locals;
	size_t nlocals;
	size_t caplocals;
	I386String *strings;
	size_t nstrings;
	size_t capstrings;
	Decl *current;
	long frame_size;
	long label_number;
	char return_label[128];
	char **break_labels;
	size_t nbreak_labels;
	size_t capbreak_labels;
	char **continue_labels;
	size_t ncontinue_labels;
	size_t capcontinue_labels;
} I386Gen;
enum
{
	I386_SEC_NULL,
	I386_SEC_TEXT,
	I386_SEC_RODATA,
	I386_SEC_DATA,
	I386_SEC_BSS,
	I386_SEC_REL_TEXT,
	I386_SEC_REL_DATA,
	I386_SEC_SYMTAB,
	I386_SEC_STRTAB,
	I386_SEC_SHSTRTAB,
	I386_SEC_COUNT
};
static long i386_type_align(CType *type);

static long i386_type_size(CType *type)
{
	switch(type->kind) {
	case TY_VOID:
		return 0;
	case TY_BOOL:
	case TY_CHAR:
	case TY_U8:
		return 1;
	case TY_SHORT:
	case TY_U16:
		return 2;
	case TY_ENUM:
	case TY_INT:
	case TY_U32:
	case TY_FLOAT:
		return 4;
	case TY_LONG:
	case TY_ULONG:
	case TY_PTR:
	case TY_OPAQUE:
		return 4;
	case TY_LLONG:
	case TY_U64:
	case TY_I64:
	case TY_DOUBLE:
		return 8;
	case TY_LDOUBLE:
		return 12;
	case TY_STRUCT:
	{
		long offset = 0;
		long maximum = 1;
		size_t index;
		for(index = 0; index < type->nmembers; index++) {
			long alignment = type->packed ? 1 : i386_type_align(type->members[index].type);
			offset = align_up(offset, alignment);
			offset += i386_type_size(type->members[index].type);
			if(alignment > maximum)
				maximum = alignment;
		}
		return type->packed ? offset : align_up(offset, maximum);
	}
	case TY_ARRAY:
		if(is_vla(type))
			return 4;
		return i386_type_size(type->base) * type->count;
	case TY_XEVENT:
		return 96;
	case TY_VALIST:
		return 4;
	}
	fatal("internal: unknown i386 type");
	return 0;
}

static long i386_type_align(CType *type)
{
	long size;
	if(type->kind == TY_ARRAY)
		return i386_type_align(type->base);
	if(type->kind == TY_STRUCT) {
		long maximum = 1;
		if(type->packed)
			return 1;
		size_t index;
		for(index = 0; index < type->nmembers; index++) {
			long alignment = i386_type_align(type->members[index].type);
			if(alignment > maximum)
				maximum = alignment;
		}
		return maximum > 4 ? 4 : maximum;
	}
	size = i386_type_size(type);
	if(size <= 1)
		return 1;
	if(size == 2)
		return 2;
	return 4;
}

static void i386_put8(Buf *buffer, uint8_t value)
{
	bputn(buffer, (char *)&value, 1);
}

static void i386_put16(Buf *buffer, uint16_t value)
{
	bputn(buffer, (char *)&value, 2);
}

static void i386_put32(Buf *buffer, uint32_t value)
{
	bputn(buffer, (char *)&value, 4);
}

static void i386_patch32(Buf *buffer, uint32_t offset, uint32_t value)
{
	if((uint64_t)offset + 4 > buffer->n)
		fatal("internal: i386 patch is outside section");
	memcpy(buffer->s + offset, &value, 4);
}

static uint32_t i386_align32(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static I386ObjSymbol *i386_find_object_symbol(I386Gen *gen,
					      const char *name)
{
	size_t index;
	for(index = 0; index < gen->nsymbols; index++)
		if(!strcmp(gen->symbols[index].name, name))
			return &gen->symbols[index];
	return NULL;
}

static I386ObjSymbol *i386_add_object_symbol(I386Gen *gen,
					     const char *name,
					     CType *type,
					     bool function)
{
	I386ObjSymbol *symbol = i386_find_object_symbol(gen, name);
	if(symbol) {
		if(function != symbol->function)
			fatal("symbol %s declared as both object and function", name);
		return symbol;
	}
	ARR_GROW(gen->symbols, gen->nsymbols, gen->capsymbols, I386ObjSymbol);
	symbol = &gen->symbols[gen->nsymbols++];
	memset(symbol, 0, sizeof(*symbol));
	symbol->name = xstrdup(name);
	symbol->type = type;
	symbol->function = function;
	return symbol;
}

static void i386_add_relocation(I386Gen *gen, uint16_t target_section, uint32_t offset, uint32_t type, const char *symbol, uint16_t section_symbol)
{
	I386Relocation *relocation;
	ARR_GROW(gen->relocations, gen->nrelocations, gen->caprelocations, I386Relocation);
	relocation = &gen->relocations[gen->nrelocations++];
	relocation->offset = offset;
	relocation->type = type;
	relocation->symbol = symbol ? xstrdup(symbol) : NULL;
	relocation->section_symbol = section_symbol;
	relocation->target_section = target_section;
}

static I386Label *i386_find_label(I386Gen *gen, const char *name)
{
	size_t index;
	for(index = 0; index < gen->nlabels; index++)
		if(!strcmp(gen->labels[index].name, name))
			return &gen->labels[index];
	return NULL;
}

static void i386_define_label(I386Gen *gen, const char *name)
{
	I386Label *label = i386_find_label(gen, name);
	if(!label) {
		ARR_GROW(gen->labels, gen->nlabels, gen->caplabels, I386Label);
		label = &gen->labels[gen->nlabels++];
		memset(label, 0, sizeof(*label));
		label->name = xstrdup(name);
	}
	if(label->defined)
		fatal("internal: duplicate i386 label %s", name);
	label->defined = true;
	label->offset = (uint32_t)gen->text.n;
}

static char *i386_new_label(I386Gen *gen, const char *prefix)
{
	char buffer[128];
	snprintf(buffer, sizeof(buffer), "%s%ld", prefix, ++gen->label_number);
	return xstrdup(buffer);
}

static void i386_add_jump_fixup(I386Gen *gen, const char *name, uint32_t offset)
{
	I386JumpFixup *fixup;
	ARR_GROW(gen->jump_fixups, gen->njump_fixups, gen->capjump_fixups, I386JumpFixup);
	fixup = &gen->jump_fixups[gen->njump_fixups++];
	fixup->name = xstrdup(name);
	fixup->offset = offset;
}

static void i386_emit_jump(I386Gen *gen, uint8_t condition, const char *label)
{
	uint32_t offset;
	if(condition == 0xff) {
		i386_put8(&gen->text, 0xe9);
	} else {
		i386_put8(&gen->text, 0x0f);
		i386_put8(&gen->text, condition);
	}
	offset = (uint32_t)gen->text.n;
	i386_put32(&gen->text, 0);
	i386_add_jump_fixup(gen, label, offset);
}

static void i386_patch_jumps(I386Gen *gen)
{
	size_t index;
	for(index = 0; index < gen->njump_fixups; index++) {
		I386JumpFixup *fixup = &gen->jump_fixups[index];
		I386Label *label = i386_find_label(gen, fixup->name);
		int64_t displacement;
		if(!label || !label->defined)
			fatal("internal: undefined i386 label %s", fixup->name);
		displacement = (int64_t)label->offset -
			       ((int64_t)fixup->offset + 4);
		i386_patch32(&gen->text, fixup->offset, (uint32_t)(int32_t)displacement);
	}
}

static I386Local *i386_find_local(I386Gen *gen, const char *name)
{
	size_t index;
	for(index = 0; index < gen->nlocals; index++)
		if(!strcmp(gen->locals[index].name, name))
			return &gen->locals[index];
	return NULL;
}

static Decl *i386_find_function(Program *program, const char *name)
{
	size_t index;
	Decl *prototype = NULL;
	for(index = 0; index < program->n; index++) {
		Decl *declaration = program->a[index];
		if(strcmp(declaration->name, name))
			continue;
		if(!(declaration->body || declaration->prototype))
			continue;
		if(declaration->body)
			return declaration;
		prototype = declaration;
	}
	return prototype;
}

static Decl *i386_find_global(Program *program, const char *name)
{
	size_t index;
	Decl *external = NULL;
	for(index = 0; index < program->n; index++) {
		Decl *declaration = program->a[index];
		if(strcmp(declaration->name, name))
			continue;
		if(declaration->body || declaration->prototype)
			continue;
		if(!declaration->is_extern)
			return declaration;
		external = declaration;
	}
	return external;
}

static StructMember *i386_find_struct_member(CType *type,
					     const char *name,
					     long *member_offset)
{
	long offset = 0;
	size_t index;
	if(!type || type->kind != TY_STRUCT)
		return NULL;
	for(index = 0; index < type->nmembers; index++) {
		long alignment = type->packed ? 1 : i386_type_align(type->members[index].type);
		offset = align_up(offset, alignment);
		if(!strcmp(type->members[index].name, name)) {
			if(member_offset)
				*member_offset = offset;
			return &type->members[index];
		}
		offset += i386_type_size(type->members[index].type);
	}
	return NULL;
}

static CType *i386_expr_type(I386Gen *gen, Expr *expression)
{
	CType *type;
	Decl *declaration;
	I386Local *local;
	if(expression->type)
		return expression->type;
	switch(expression->kind) {
	case EX_ID:
		local = i386_find_local(gen, expression->str);
		if(local)
			return local->type;
		declaration = i386_find_global(gen->program, expression->str);
		if(declaration)
			return declaration->type;
		declaration = i386_find_function(gen->program, expression->str);
		if(declaration)
			return declaration->type;
		return &T_U32;
	case EX_STR:
		return ptr_to(&T_CHAR);
	case EX_ENUM_CONST:
	case EX_NUM:
		return &T_U32;
	case EX_UNARY:
		if(!strcmp(expression->op, "&"))
			return ptr_to(i386_expr_type(gen, expression->left));
		if(!strcmp(expression->op, "*")) {
			type = i386_expr_type(gen, expression->left);
			return type->base ? type->base : &T_U32;
		}
		return i386_expr_type(gen, expression->left);
	case EX_INDEX:
		type = i386_expr_type(gen, expression->left);
		return type->base ? type->base : &T_U32;
	case EX_MEMBER:
	case EX_PTRMEMBER:
	{
		StructMember *member;
		type = i386_expr_type(gen, expression->left);
		if(expression->kind == EX_PTRMEMBER)
			type = type && type->kind == TY_PTR ? type->base : NULL;
		member = i386_find_struct_member(type, expression->str, NULL);
		if(!member)
			fatal("unknown struct member %s", expression->str);
		return member->type;
	}
	case EX_SIZEOF:
		return &T_U32;
	case EX_TYPEOF:
		return expression->sizeof_type ? expression->sizeof_type : i386_expr_type(gen, expression->left);
	case EX_TYPE:
		return expression->type;
	case EX_BINARY:
		if(!strcmp(expression->op, "==") ||
		   !strcmp(expression->op, "!=") ||
		   !strcmp(expression->op, "<") ||
		   !strcmp(expression->op, "<=") ||
		   !strcmp(expression->op, ">") ||
		   !strcmp(expression->op, ">=") ||
		   !strcmp(expression->op, "&&") ||
		   !strcmp(expression->op, "||"))
			return &T_INT;
		return i386_expr_type(gen, expression->left);
	case EX_CALL:
		if(expression->left->kind == EX_ID) {
			if(!strcmp(expression->left->str, "input"))
				return ptr_to(&T_CHAR);
			declaration = i386_find_function(gen->program,
							 expression->left->str);
			if(declaration)
				return declaration->type;
		}
		return &T_U32;
	case EX_INITLIST:
		return &T_VOID;
	case EX_MATCH:
		return &T_INT;
	}
	return &T_U32;
}

static uint32_t i386_intern_string(I386Gen *gen, const char *value)
{
	size_t index;
	uint32_t offset;
	for(index = 0; index < gen->nstrings; index++)
		if(!strcmp(gen->strings[index].value, value))
			return gen->strings[index].offset;
	offset = (uint32_t)gen->rodata.n;
	bputn(&gen->rodata, value, strlen(value) + 1);
	ARR_GROW(gen->strings, gen->nstrings, gen->capstrings, I386String);
	gen->strings[gen->nstrings].value = xstrdup(value);
	gen->strings[gen->nstrings].offset = offset;
	gen->nstrings++;
	return offset;
}

static void i386_emit_mov_eax_imm(I386Gen *gen, uint32_t value)
{
	i386_put8(&gen->text, 0xb8);
	i386_put32(&gen->text, value);
}

static void i386_emit_mov_eax_symbol(I386Gen *gen, const char *name)
{
	uint32_t offset;
	i386_put8(&gen->text, 0xb8);
	offset = (uint32_t)gen->text.n;
	i386_put32(&gen->text, 0);
	i386_add_relocation(gen, I386_SEC_TEXT, offset, R_386_32, name, 0);
}

static void i386_emit_mov_eax_rodata(I386Gen *gen, uint32_t addend)
{
	uint32_t offset;
	i386_put8(&gen->text, 0xb8);
	offset = (uint32_t)gen->text.n;
	i386_put32(&gen->text, addend);
	i386_add_relocation(gen, I386_SEC_TEXT, offset, R_386_32, NULL, I386_SEC_RODATA);
}

static void i386_emit_lea_ebp(I386Gen *gen, long displacement)
{
	i386_put8(&gen->text, 0x8d);
	i386_put8(&gen->text, 0x85);
	i386_put32(&gen->text, (uint32_t)(int32_t)displacement);
}

static void i386_emit_load_ebp(I386Gen *gen, CType *type, long displacement)
{
	long size = i386_type_size(type);
	if(type->kind == TY_ARRAY || type->kind == TY_STRUCT ||
	   type->kind == TY_XEVENT || type->kind == TY_VALIST)
	{
		i386_emit_lea_ebp(gen, displacement);
		return;
	}
	if(size == 1) {
		i386_put8(&gen->text, 0x0f);
		i386_put8(&gen->text, 0xb6);
		i386_put8(&gen->text, 0x85);
		i386_put32(&gen->text, (uint32_t)(int32_t)displacement);
	} else if(size == 2)
	{
		i386_put8(&gen->text, 0x0f);
		i386_put8(&gen->text, 0xb7);
		i386_put8(&gen->text, 0x85);
		i386_put32(&gen->text, (uint32_t)(int32_t)displacement);
	} else if(size == 8)
	{
		i386_put8(&gen->text, 0x8b);
		i386_put8(&gen->text, 0x85);
		i386_put32(&gen->text, (uint32_t)(int32_t)displacement);
		i386_put8(&gen->text, 0x8b);
		i386_put8(&gen->text, 0x95);
		i386_put32(&gen->text,
			   (uint32_t)(int32_t)(displacement + 4));
	} else {
		i386_put8(&gen->text, 0x8b);
		i386_put8(&gen->text, 0x85);
		i386_put32(&gen->text, (uint32_t)(int32_t)displacement);
	}
}

static void i386_emit_load_ecx_address(I386Gen *gen, CType *type)
{
	long size = i386_type_size(type);
	if(type->kind == TY_ARRAY || type->kind == TY_STRUCT ||
	   type->kind == TY_XEVENT || type->kind == TY_VALIST)
		return;
	if(size == 1) {
		i386_put8(&gen->text, 0x0f);
		i386_put8(&gen->text, 0xb6);
		i386_put8(&gen->text, 0x01);
	} else if(size == 2)
	{
		i386_put8(&gen->text, 0x0f);
		i386_put8(&gen->text, 0xb7);
		i386_put8(&gen->text, 0x01);
	} else {
		i386_put8(&gen->text, 0x8b);
		i386_put8(&gen->text, 0x01);
	}
}

static void i386_emit_load_symbol(I386Gen *gen, const char *name, CType *type)
{
	long size = i386_type_size(type);
	uint32_t offset;
	if(type->kind == TY_ARRAY || type->kind == TY_STRUCT ||
	   type->kind == TY_XEVENT || type->kind == TY_VALIST)
	{
		i386_emit_mov_eax_symbol(gen, name);
		return;
	}
	if(size == 1) {
		i386_put8(&gen->text, 0x0f);
		i386_put8(&gen->text, 0xb6);
		i386_put8(&gen->text, 0x05);
		offset = (uint32_t)gen->text.n;
		i386_put32(&gen->text, 0);
	} else if(size == 2)
	{
		i386_put8(&gen->text, 0x0f);
		i386_put8(&gen->text, 0xb7);
		i386_put8(&gen->text, 0x05);
		offset = (uint32_t)gen->text.n;
		i386_put32(&gen->text, 0);
	} else if(size == 8)
	{
		i386_put8(&gen->text, 0xa1);
		offset = (uint32_t)gen->text.n;
		i386_put32(&gen->text, 0);
		i386_add_relocation(gen, I386_SEC_TEXT, offset, R_386_32, name, 0);
		i386_put8(&gen->text, 0x8b);
		i386_put8(&gen->text, 0x15);
		offset = (uint32_t)gen->text.n;
		i386_put32(&gen->text, 4);
		i386_add_relocation(gen, I386_SEC_TEXT, offset, R_386_32, name, 0);
		return;
	} else {
		i386_put8(&gen->text, 0xa1);
		offset = (uint32_t)gen->text.n;
		i386_put32(&gen->text, 0);
	}
	i386_add_relocation(gen, I386_SEC_TEXT, offset, R_386_32, name, 0);
}

static void i386_emit_store_ecx(I386Gen *gen, CType *type)
{
	long size = i386_type_size(type);
	if(size == 1) {
		i386_put8(&gen->text, 0x88);
		i386_put8(&gen->text, 0x01);
	} else if(size == 2)
	{
		i386_put8(&gen->text, 0x66);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0x01);
	} else if(size == 8)
	{
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0x01);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0x51);
		i386_put8(&gen->text, 0x04);
	} else {
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0x01);
	}
}
static void i386_gen_expression(I386Gen *gen, Expr *expression);

static void i386_emit_load_vla_pointer(I386Gen *gen, I386Local *local)
{
	i386_put8(&gen->text, 0x8b);
	i386_put8(&gen->text, 0x85);
	i386_put32(&gen->text, (uint32_t)(int32_t)local->offset);
}

static void i386_emit_vla_bound(I386Gen *gen, const char *name)
{
	I386Local *local = i386_find_local(gen, name);
	Decl *global;
	if(local) {
		i386_emit_load_ebp(gen, local->type, local->offset);
		return;
	}
	global = i386_find_global(gen->program, name);
	if(global) {
		i386_emit_load_symbol(gen, global->name, global->type);
		return;
	}
	i386_add_object_symbol(gen, name, &T_U32, false);
	i386_emit_load_symbol(gen, name, &T_U32);
}

static CType *i386_gen_address(I386Gen *gen, Expr *expression)
{
	I386Local *local;
	Decl *global;
	CType *type;
	long size;
	if(expression->kind == EX_ID) {
		local = i386_find_local(gen, expression->str);
		if(local) {
			if(is_vla(local->type))
				i386_emit_load_vla_pointer(gen, local);
			else
				i386_emit_lea_ebp(gen, local->offset);
			return local->type;
		}
		global = i386_find_global(gen->program, expression->str);
		if(global) {
			i386_emit_mov_eax_symbol(gen, global->name);
			return global->type;
		}
		global = i386_find_function(gen->program, expression->str);
		if(global) {
			i386_emit_mov_eax_symbol(gen, global->name);
			return global->type;
		}
		fatal("%s is not addressable", expression->str);
	}
	if(expression->kind == EX_UNARY &&
	   !strcmp(expression->op, "*"))
	{
		i386_gen_expression(gen, expression->left);
		type = i386_expr_type(gen, expression->left);
		return type->base ? type->base : &T_U32;
	}
	if(expression->kind == EX_INDEX) {
		i386_gen_expression(gen, expression->left);
		i386_put8(&gen->text, 0x50);
		i386_gen_expression(gen, expression->right);
		type = i386_expr_type(gen, expression->left);
		type = type->base ? type->base : &T_U32;
		size = i386_type_size(type);
		if(size != 1) {
			i386_put8(&gen->text, 0x69);
			i386_put8(&gen->text, 0xc0);
			i386_put32(&gen->text, (uint32_t)size);
		}
		i386_put8(&gen->text, 0x59);
		i386_put8(&gen->text, 0x01);
		i386_put8(&gen->text, 0xc8);
		return type;
	}
	if(expression->kind == EX_MEMBER ||
	   expression->kind == EX_PTRMEMBER)
	{
		StructMember *member;
		CType *owner;
		long member_offset;
		if(expression->kind == EX_MEMBER) {
			i386_gen_address(gen, expression->left);
			owner = i386_expr_type(gen, expression->left);
		} else {
			i386_gen_expression(gen, expression->left);
			owner = i386_expr_type(gen, expression->left);
			owner = owner && owner->kind == TY_PTR ? owner->base : NULL;
		}
		member = i386_find_struct_member(owner, expression->str, &member_offset);
		if(!member)
			fatal("unknown struct member %s", expression->str);
		if(member_offset) {
			i386_put8(&gen->text, 0x05);
			i386_put32(&gen->text, (uint32_t)member_offset);
		}
		return member->type;
	}
	fatal("expression is not an lvalue");
	return &T_U32;
}

static void i386_emit_setcc(I386Gen *gen, uint8_t condition)
{
	i386_put8(&gen->text, 0x0f);
	i386_put8(&gen->text, condition);
	i386_put8(&gen->text, 0xc0);
	i386_put8(&gen->text, 0x0f);
	i386_put8(&gen->text, 0xb6);
	i386_put8(&gen->text, 0xc0);
}

static bool i386_type_is_unsigned(CType *type)
{
	if(!type)
		return false;
	return type->kind == TY_U8 || type->kind == TY_U16 ||
	       type->kind == TY_U32 || type->kind == TY_U64 ||
	       type->kind == TY_PTR || type->kind == TY_ARRAY;
}

static bool i386_expression_is_unsigned(I386Gen *gen, Expr *expression)
{
	return i386_type_is_unsigned(i386_expr_type(gen, expression));
}

static void i386_emit_division(I386Gen *gen, bool is_unsigned)
{
	/*
	Entry: ECX = dividend, EAX = divisor.
	ECDQ overwrites EDX, so the divisor must not live in EDX when CDQ
	executes.  Version 1.5 used IDIV EDX after CDQ and therefore
	divided by zero for every positive dividend.
	*/
	i386_put8(&gen->text, 0x89);
	i386_put8(&gen->text, 0xc2);
	i386_put8(&gen->text, 0x89);
	i386_put8(&gen->text, 0xc8);
	i386_put8(&gen->text, 0x89);
	i386_put8(&gen->text, 0xd1);
	if(is_unsigned) {
		i386_put8(&gen->text, 0x31);
		i386_put8(&gen->text, 0xd2);
		i386_put8(&gen->text, 0xf7);
		i386_put8(&gen->text, 0xf1);
	} else {
		i386_put8(&gen->text, 0x99);
		i386_put8(&gen->text, 0xf7);
		i386_put8(&gen->text, 0xf9);
	}
}

static bool i386_expression_is_u64(I386Gen *gen, Expr *expression)
{
	CType *type = i386_expr_type(gen, expression);
	return type && type->kind == TY_U64;
}

static void i386_emit_zero_edx(I386Gen *gen)
{
	i386_put8(&gen->text, 0x31);
	i386_put8(&gen->text, 0xd2);
}

static void i386_emit_mul64_imm(I386Gen *gen,
				unsigned long long multiplier)
{
	uint32_t low = (uint32_t)multiplier;
	uint32_t high = (uint32_t)(multiplier >> 32);
	//Preserve the original halves and form the low 64 bits of the product.
	i386_put8(&gen->text, 0x50);
	i386_put8(&gen->text, 0x52);
	i386_put8(&gen->text, 0xb9);
	i386_put32(&gen->text, low);
	i386_put8(&gen->text, 0xf7);
	i386_put8(&gen->text, 0xe1);
	i386_put8(&gen->text, 0x50);
	i386_put8(&gen->text, 0x52);
	//ECX = high32(original_low * low).
	i386_put8(&gen->text, 0x8b);
	i386_put8(&gen->text, 0x0c);
	i386_put8(&gen->text, 0x24);
	//Add original_high * low.
	i386_put8(&gen->text, 0x8b);
	i386_put8(&gen->text, 0x44);
	i386_put8(&gen->text, 0x24);
	i386_put8(&gen->text, 0x08);
	i386_put8(&gen->text, 0x69);
	i386_put8(&gen->text, 0xc0);
	i386_put32(&gen->text, low);
	i386_put8(&gen->text, 0x01);
	i386_put8(&gen->text, 0xc1);
	//Add original_low * high.
	i386_put8(&gen->text, 0x8b);
	i386_put8(&gen->text, 0x44);
	i386_put8(&gen->text, 0x24);
	i386_put8(&gen->text, 0x0c);
	i386_put8(&gen->text, 0x69);
	i386_put8(&gen->text, 0xc0);
	i386_put32(&gen->text, high);
	i386_put8(&gen->text, 0x01);
	i386_put8(&gen->text, 0xc1);
	//Restore result low and install result high.
	i386_put8(&gen->text, 0x8b);
	i386_put8(&gen->text, 0x44);
	i386_put8(&gen->text, 0x24);
	i386_put8(&gen->text, 0x04);
	i386_put8(&gen->text, 0x89);
	i386_put8(&gen->text, 0xca);
	i386_put8(&gen->text, 0x83);
	i386_put8(&gen->text, 0xc4);
	i386_put8(&gen->text, 0x10);
}

static void i386_emit_shift64_imm(I386Gen *gen, bool left, unsigned int count)
{
	count &= 63;
	if(!count)
		return;
	if(left) {
		if(count < 32) {
			i386_put8(&gen->text, 0x0f);
			i386_put8(&gen->text, 0xa4);
			i386_put8(&gen->text, 0xc2);
			i386_put8(&gen->text, (uint8_t)count);
			i386_put8(&gen->text, 0xc1);
			i386_put8(&gen->text, 0xe0);
			i386_put8(&gen->text, (uint8_t)count);
		} else if(count == 32)
		{
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xc2);
			i386_put8(&gen->text, 0x31);
			i386_put8(&gen->text, 0xc0);
		} else {
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xc2);
			i386_put8(&gen->text, 0xc1);
			i386_put8(&gen->text, 0xe2);
			i386_put8(&gen->text, (uint8_t)(count - 32));
			i386_put8(&gen->text, 0x31);
			i386_put8(&gen->text, 0xc0);
		}
	} else {
		if(count < 32) {
			i386_put8(&gen->text, 0x0f);
			i386_put8(&gen->text, 0xac);
			i386_put8(&gen->text, 0xd0);
			i386_put8(&gen->text, (uint8_t)count);
			i386_put8(&gen->text, 0xc1);
			i386_put8(&gen->text, 0xea);
			i386_put8(&gen->text, (uint8_t)count);
		} else if(count == 32)
		{
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xd0);
			i386_emit_zero_edx(gen);
		} else {
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xd0);
			i386_put8(&gen->text, 0xc1);
			i386_put8(&gen->text, 0xe8);
			i386_put8(&gen->text, (uint8_t)(count - 32));
			i386_emit_zero_edx(gen);
		}
	}
}

static void i386_gen_binary64(I386Gen *gen, Expr *expression)
{
	const char *operator= expression->op;
	CType *type;
	if(!strcmp(operator, "=")) {
		type = i386_gen_address(gen, expression->left);
		i386_put8(&gen->text, 0x50);
		i386_gen_expression(gen, expression->right);
		if(!i386_expression_is_u64(gen, expression->right))
			i386_emit_zero_edx(gen);
		i386_put8(&gen->text, 0x59);
		i386_emit_store_ecx(gen, type);
		return;
	}
	if(!strcmp(operator, "^=") || !strcmp(operator, "|=") ||
	   !strcmp(operator, "&=") || !strcmp(operator, "*="))
	{
		type = i386_gen_address(gen, expression->left);
		i386_put8(&gen->text, 0x50);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0xc1);
		i386_put8(&gen->text, 0x8b);
		i386_put8(&gen->text, 0x01);
		i386_put8(&gen->text, 0x8b);
		i386_put8(&gen->text, 0x51);
		i386_put8(&gen->text, 0x04);
		if(!strcmp(operator, "*=")) {
			if(expression->right->kind != EX_NUM)
				fatal("64-bit compound multiplication requires a constant");
			i386_emit_mul64_imm(gen, expression->right->num);
		} else {
			i386_put8(&gen->text, 0x53);
			i386_put8(&gen->text, 0x52);
			i386_put8(&gen->text, 0x50);
			i386_gen_expression(gen, expression->right);
			if(!i386_expression_is_u64(gen, expression->right))
				i386_emit_zero_edx(gen);
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xd3);
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xc1);
			i386_put8(&gen->text, 0x58);
			i386_put8(&gen->text, 0x5a);
			if(!strcmp(operator, "^=")) {
				i386_put8(&gen->text, 0x31);
				i386_put8(&gen->text, 0xc8);
				i386_put8(&gen->text, 0x31);
				i386_put8(&gen->text, 0xda);
			} else if(!strcmp(operator, "|="))
			{
				i386_put8(&gen->text, 0x09);
				i386_put8(&gen->text, 0xc8);
				i386_put8(&gen->text, 0x09);
				i386_put8(&gen->text, 0xda);
			} else {
				i386_put8(&gen->text, 0x21);
				i386_put8(&gen->text, 0xc8);
				i386_put8(&gen->text, 0x21);
				i386_put8(&gen->text, 0xda);
			}
			i386_put8(&gen->text, 0x5b);
		}
		i386_put8(&gen->text, 0x59);
		i386_emit_store_ecx(gen, type);
		return;
	}
	if(!strcmp(operator, "<<") || !strcmp(operator, ">>")) {
		if(expression->right->kind != EX_NUM)
			fatal("64-bit shifts require a constant count");
		i386_gen_expression(gen, expression->left);
		if(!i386_expression_is_u64(gen, expression->left))
			i386_emit_zero_edx(gen);
		i386_emit_shift64_imm(gen, !strcmp(operator, "<<"), (unsigned int)expression->right->num);
		return;
	}
	if(!strcmp(operator, "*") && expression->right->kind == EX_NUM) {
		i386_gen_expression(gen, expression->left);
		if(!i386_expression_is_u64(gen, expression->left))
			i386_emit_zero_edx(gen);
		i386_emit_mul64_imm(gen, expression->right->num);
		return;
	}
	if(!strcmp(operator, "|") || !strcmp(operator, "^") ||
	   !strcmp(operator, "&") || !strcmp(operator, "+") ||
	   !strcmp(operator, "-"))
	{
		i386_put8(&gen->text, 0x53);
		i386_gen_expression(gen, expression->left);
		if(!i386_expression_is_u64(gen, expression->left))
			i386_emit_zero_edx(gen);
		i386_put8(&gen->text, 0x52);
		i386_put8(&gen->text, 0x50);
		i386_gen_expression(gen, expression->right);
		if(!i386_expression_is_u64(gen, expression->right))
			i386_emit_zero_edx(gen);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0xd3);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0xc1);
		i386_put8(&gen->text, 0x58);
		i386_put8(&gen->text, 0x5a);
		if(!strcmp(operator, "|")) {
			i386_put8(&gen->text, 0x09);
			i386_put8(&gen->text, 0xc8);
			i386_put8(&gen->text, 0x09);
			i386_put8(&gen->text, 0xda);
		} else if(!strcmp(operator, "^"))
		{
			i386_put8(&gen->text, 0x31);
			i386_put8(&gen->text, 0xc8);
			i386_put8(&gen->text, 0x31);
			i386_put8(&gen->text, 0xda);
		} else if(!strcmp(operator, "&"))
		{
			i386_put8(&gen->text, 0x21);
			i386_put8(&gen->text, 0xc8);
			i386_put8(&gen->text, 0x21);
			i386_put8(&gen->text, 0xda);
		} else if(!strcmp(operator, "+"))
		{
			i386_put8(&gen->text, 0x01);
			i386_put8(&gen->text, 0xc8);
			i386_put8(&gen->text, 0x11);
			i386_put8(&gen->text, 0xda);
		} else {
			i386_put8(&gen->text, 0x29);
			i386_put8(&gen->text, 0xc8);
			i386_put8(&gen->text, 0x19);
			i386_put8(&gen->text, 0xda);
		}
		i386_put8(&gen->text, 0x5b);
		return;
	}
	fatal("unsupported 64-bit i386 operator %s", operator);
}

static CType *i386_type_operand(I386Gen *gen, Expr *expression)
{
	if(!expression)
		return NULL;
	if(expression->kind == EX_TYPE)
		return expression->type;
	if(expression->kind == EX_TYPEOF)
		return expression->sizeof_type ? expression->sizeof_type : i386_expr_type(gen, expression->left);
	return NULL;
}

static void i386_gen_binary(I386Gen *gen, Expr *expression)
{
	const char *operator= expression->op;
	CType *type;
	CType *left_type = i386_type_operand(gen, expression->left);
	CType *right_type = i386_type_operand(gen, expression->right);
	if((!strcmp(operator, "==") || !strcmp(operator, "!=")) && left_type && right_type) {
		bool equal = type_equal(left_type, right_type);
		i386_emit_mov_eax_imm(gen, !strcmp(operator, "==") ? equal : !equal);
		return;
	}
	if(i386_expression_is_u64(gen, expression->left) &&
	   strcmp(operator, "==") && strcmp(operator, "!=") &&
	   strcmp(operator, "<") && strcmp(operator, "<=") &&
	   strcmp(operator, ">") && strcmp(operator, ">="))
	{
		i386_gen_binary64(gen, expression);
		return;
	}
	if(!strcmp(operator, "=")) {
		type = i386_gen_address(gen, expression->left);
		i386_put8(&gen->text, 0x50);
		i386_gen_expression(gen, expression->right);
		if(type->kind == TY_STRUCT) {
			long size = i386_type_size(type);
			i386_put8(&gen->text, 0x5a);
			i386_put8(&gen->text, 0x52);
			i386_put8(&gen->text, 0x56);
			i386_put8(&gen->text, 0x57);
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xc6);
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xd7);
			i386_put8(&gen->text, 0xb9);
			i386_put32(&gen->text, (uint32_t)size);
			i386_put8(&gen->text, 0xfc);
			i386_put8(&gen->text, 0xf3);
			i386_put8(&gen->text, 0xa4);
			i386_put8(&gen->text, 0x5f);
			i386_put8(&gen->text, 0x5e);
			i386_put8(&gen->text, 0x58);
		} else {
			i386_put8(&gen->text, 0x59);
			i386_emit_store_ecx(gen, type);
		}
		return;
	}
	if(!strcmp(operator, "+=") || !strcmp(operator, "-=") ||
	   !strcmp(operator, "*=") || !strcmp(operator, "/=") ||
	   !strcmp(operator, "%=") || !strcmp(operator, "&=") ||
	   !strcmp(operator, "|=") || !strcmp(operator, "^="))
	{
		type = i386_gen_address(gen, expression->left);
		i386_put8(&gen->text, 0x50);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0xc1);
		i386_emit_load_ecx_address(gen, type);
		i386_put8(&gen->text, 0x50);
		i386_gen_expression(gen, expression->right);
		i386_put8(&gen->text, 0x59);
		if(!strcmp(operator, "+=")) {
			i386_put8(&gen->text, 0x01);
			i386_put8(&gen->text, 0xc8);
		} else if(!strcmp(operator, "-="))
		{
			i386_put8(&gen->text, 0x29);
			i386_put8(&gen->text, 0xc1);
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xc8);
		} else if(!strcmp(operator, "*="))
		{
			i386_put8(&gen->text, 0x0f);
			i386_put8(&gen->text, 0xaf);
			i386_put8(&gen->text, 0xc1);
		} else if(!strcmp(operator, "&="))
		{
			i386_put8(&gen->text, 0x21);
			i386_put8(&gen->text, 0xc8);
		} else if(!strcmp(operator, "|="))
		{
			i386_put8(&gen->text, 0x09);
			i386_put8(&gen->text, 0xc8);
		} else if(!strcmp(operator, "^="))
		{
			i386_put8(&gen->text, 0x31);
			i386_put8(&gen->text, 0xc8);
		} else {
			i386_emit_division(gen,
					   i386_type_is_unsigned(type) ||
						   i386_expression_is_unsigned(gen,
									       expression->right));
			if(!strcmp(operator, "%=")) {
				i386_put8(&gen->text, 0x89);
				i386_put8(&gen->text, 0xd0);
			}
		}
		i386_put8(&gen->text, 0x59);
		i386_emit_store_ecx(gen, type);
		return;
	}
	if(!strcmp(operator, "&&") || !strcmp(operator, "||")) {
		char *short_label = i386_new_label(gen, ".Llogic");
		char *done_label = i386_new_label(gen, ".Llogic_done");
		i386_gen_expression(gen, expression->left);
		i386_put8(&gen->text, 0x85);
		i386_put8(&gen->text, 0xc0);
		i386_emit_jump(gen, !strcmp(operator, "&&") ? 0x84 : 0x85, short_label);
		i386_gen_expression(gen, expression->right);
		i386_put8(&gen->text, 0x85);
		i386_put8(&gen->text, 0xc0);
		i386_emit_setcc(gen, 0x95);
		i386_emit_jump(gen, 0xff, done_label);
		i386_define_label(gen, short_label);
		i386_emit_mov_eax_imm(gen, !strcmp(operator, "&&") ? 0 : 1);
		i386_define_label(gen, done_label);
		return;
	}
	i386_gen_expression(gen, expression->left);
	i386_put8(&gen->text, 0x50);
	i386_gen_expression(gen, expression->right);
	i386_put8(&gen->text, 0x59);
	if(!strcmp(operator, "+")) {
		i386_put8(&gen->text, 0x01);
		i386_put8(&gen->text, 0xc8);
	} else if(!strcmp(operator, "-"))
	{
		i386_put8(&gen->text, 0x29);
		i386_put8(&gen->text, 0xc1);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0xc8);
	} else if(!strcmp(operator, "*"))
	{
		i386_put8(&gen->text, 0x0f);
		i386_put8(&gen->text, 0xaf);
		i386_put8(&gen->text, 0xc1);
	} else if(!strcmp(operator, "/") || !strcmp(operator, "%"))
	{
		i386_emit_division(gen,
				   i386_expression_is_unsigned(gen, expression->left) ||
					   i386_expression_is_unsigned(gen, expression->right));
		if(!strcmp(operator, "%")) {
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xd0);
		}
	} else if(!strcmp(operator, "&"))
	{
		i386_put8(&gen->text, 0x21);
		i386_put8(&gen->text, 0xc8);
	} else if(!strcmp(operator, "|"))
	{
		i386_put8(&gen->text, 0x09);
		i386_put8(&gen->text, 0xc8);
	} else if(!strcmp(operator, "^"))
	{
		i386_put8(&gen->text, 0x31);
		i386_put8(&gen->text, 0xc8);
	} else if(!strcmp(operator, "<<") || !strcmp(operator, ">>"))
	{
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0xc2);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0xc8);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0xd1);
		i386_put8(&gen->text, 0xd3);
		if(!strcmp(operator, "<<"))
			i386_put8(&gen->text, 0xe0);
		else
			i386_put8(&gen->text,
				  i386_expression_is_unsigned(gen, expression->left) ? 0xe8 : 0xf8);
	} else if(!strcmp(operator, "==") || !strcmp(operator, "!=") ||
		  !strcmp(operator, "<") || !strcmp(operator, "<=") ||
		  !strcmp(operator, ">") || !strcmp(operator, ">="))
	{
		uint8_t condition;
		i386_put8(&gen->text, 0x39);
		i386_put8(&gen->text, 0xc1);
		if(!strcmp(operator, "=="))
			condition = 0x94;
		else if(!strcmp(operator, "!="))
			condition = 0x95;
		else if(i386_expression_is_unsigned(gen, expression->left) ||
			i386_expression_is_unsigned(gen, expression->right))
			condition = !strcmp(operator, "<") ? 0x92 : !strcmp(operator, "<=") ? 0x96
							    : !strcmp(operator, ">")	    ? 0x97
											    : 0x93;
		else
			condition = !strcmp(operator, "<") ? 0x9c : !strcmp(operator, "<=") ? 0x9e
							    : !strcmp(operator, ">")	    ? 0x9f
											    : 0x9d;
		i386_emit_setcc(gen, condition);
	} else {
		fatal("unsupported i386 operator %s", operator);
	}
}

static void i386_gen_input(I386Gen *gen, Expr *expression)
{
	if(expression->nargs != 1)
		fatal("input expects one argument");
	i386_gen_expression(gen, expression->args[0]);
	i386_put8(&gen->text, 0x50);
	i386_put8(&gen->text, 0xe8);
	{
		uint32_t offset = (uint32_t)gen->text.n;
		i386_put32(&gen->text, 0xfffffffcU);
		i386_add_relocation(gen, I386_SEC_TEXT, offset, R_386_PC32, "printf", 0);
	}
	i386_put8(&gen->text, 0x83);
	i386_put8(&gen->text, 0xc4);
	i386_put8(&gen->text, 4);
	i386_emit_mov_eax_symbol(gen, "__modulon_input_buffer");
	i386_put8(&gen->text, 0x50);
	i386_emit_mov_eax_imm(gen, 1024);
	i386_put8(&gen->text, 0x50);
	i386_add_object_symbol(gen, "stdin", ptr_to(&T_VOID), false);
	i386_emit_load_symbol(gen, "stdin", ptr_to(&T_VOID));
	i386_put8(&gen->text, 0x50);
	i386_put8(&gen->text, 0xe8);
	{
		uint32_t offset = (uint32_t)gen->text.n;
		i386_put32(&gen->text, 0xfffffffcU);
		i386_add_relocation(gen, I386_SEC_TEXT, offset, R_386_PC32, "fgets", 0);
	}
	i386_put8(&gen->text, 0x83);
	i386_put8(&gen->text, 0xc4);
	i386_put8(&gen->text, 12);
	i386_emit_mov_eax_symbol(gen, "__modulon_input_buffer");
}

static const char *i386_variable_format(CType *type)
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

static void i386_expand_variable_formats(I386Gen *gen, Expr *expression)
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
				const char *replacement = i386_variable_format(i386_expr_type(gen, expression->args[variable_index++]));
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

static void i386_gen_call(I386Gen *gen, Expr *expression)
{
	size_t index;
	const char *name;
	uint32_t offset;
	if(expression->left->kind != EX_ID)
		fatal("only direct calls are supported by i386 backend");
	name = expression->left->str;
	if(!strcmp(name, "printf"))
		i386_expand_variable_formats(gen, expression);
	if(!strcmp(name, "input")) {
		i386_add_object_symbol(gen, "__modulon_input_buffer", ptr_to(&T_CHAR), false);
		if(!i386_find_object_symbol(gen, "__modulon_input_buffer")->defined) {
			I386ObjSymbol *symbol = i386_find_object_symbol(gen, "__modulon_input_buffer");
			symbol->defined = true;
			symbol->section = I386_SEC_BSS;
			symbol->value = gen->bss_size;
			symbol->size = 1024;
			gen->bss_size += 1024;
		}
		i386_gen_input(gen, expression);
		return;
	}
	if(!strcmp(name, "va_start") || !strcmp(name, "va_end"))
		fatal("variadic builtins are not supported by i386 backend yet");
	for(index = expression->nargs; index > 0; index--) {
		i386_gen_expression(gen, expression->args[index - 1]);
		i386_put8(&gen->text, 0x50);
	}
	i386_put8(&gen->text, 0xe8);
	offset = (uint32_t)gen->text.n;
	i386_put32(&gen->text, 0xfffffffcU);
	i386_add_relocation(gen, I386_SEC_TEXT, offset, R_386_PC32, name, 0);
	if(expression->nargs) {
		i386_put8(&gen->text, 0x81);
		i386_put8(&gen->text, 0xc4);
		i386_put32(&gen->text,
			   (uint32_t)(expression->nargs * 4));
	}
}

static void i386_gen_incdec(I386Gen *gen, Expr *expression)
{
	CType *type = i386_gen_address(gen, expression->left);
	long step = 1;
	bool postfix = !strcmp(expression->op, "post++") ||
		       !strcmp(expression->op, "post--");
	bool decrement = !strcmp(expression->op, "--") ||
			 !strcmp(expression->op, "post--");
	if(type->kind == TY_PTR && type->base) {
		step = i386_type_size(type->base);
		if(step <= 0)
			step = 1;
	}
	i386_put8(&gen->text, 0x50);
	i386_put8(&gen->text, 0x89);
	i386_put8(&gen->text, 0xc1);
	i386_emit_load_ecx_address(gen, type);
	if(postfix)
		i386_put8(&gen->text, 0x50);
	i386_put8(&gen->text, decrement ? 0x2d : 0x05);
	i386_put32(&gen->text, (uint32_t)step);
	if(postfix) {
		i386_put8(&gen->text, 0x5a);
		i386_put8(&gen->text, 0x59);
		i386_emit_store_ecx(gen, type);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0xd0);
	} else {
		i386_put8(&gen->text, 0x59);
		i386_emit_store_ecx(gen, type);
	}
}

static void i386_gen_expression(I386Gen *gen, Expr *expression)
{
	I386Local *local;
	Decl *declaration;
	CType *type;
	uint32_t string_offset;
	switch(expression->kind) {
	case EX_ENUM_CONST:
	case EX_NUM:
		i386_emit_mov_eax_imm(gen, (uint32_t)expression->num);
		if(i386_expression_is_u64(gen, expression)) {
			i386_put8(&gen->text, 0xba);
			i386_put32(&gen->text, (uint32_t)(expression->num >> 32));
		}
		return;
	case EX_STR:
		string_offset = i386_intern_string(gen, expression->str);
		i386_emit_mov_eax_rodata(gen, string_offset);
		return;
	case EX_ID:
		if(!strcmp(expression->str, "NULL")) {
			i386_emit_mov_eax_imm(gen, 0);
			return;
		}
		local = i386_find_local(gen, expression->str);
		if(local) {
			if(is_vla(local->type))
				i386_emit_load_vla_pointer(gen, local);
			else
				i386_emit_load_ebp(gen, local->type, local->offset);
			return;
		}
		declaration = i386_find_global(gen->program, expression->str);
		if(declaration) {
			i386_emit_load_symbol(gen, declaration->name, declaration->type);
			return;
		}
		declaration = i386_find_function(gen->program, expression->str);
		if(declaration) {
			i386_emit_mov_eax_symbol(gen, declaration->name);
			return;
		}
		fatal("unknown identifier %s in i386 backend", expression->str);
		return;
	case EX_SIZEOF:
		i386_emit_mov_eax_imm(gen,
				      (uint32_t)i386_type_size(expression->sizeof_type ? expression->sizeof_type : i386_expr_type(gen, expression->left)));
		return;
	case EX_TYPEOF:
	case EX_TYPE:
		fatal("type expression cannot be used as a runtime value");
		return;
	case EX_UNARY:
		if(!strcmp(expression->op, "++") ||
		   !strcmp(expression->op, "--") ||
		   !strcmp(expression->op, "post++") ||
		   !strcmp(expression->op, "post--"))
		{
			i386_gen_incdec(gen, expression);
		} else if(!strcmp(expression->op, "cast"))
		{
			bool source_wide = i386_expression_is_u64(gen, expression->left);
			i386_gen_expression(gen, expression->left);
			if(i386_type_size(expression->type) == 1) {
				i386_put8(&gen->text, 0x25);
				i386_put32(&gen->text, 0xff);
			} else if(i386_type_size(expression->type) == 2)
			{
				i386_put8(&gen->text, 0x25);
				i386_put32(&gen->text, 0xffff);
			} else if(i386_type_size(expression->type) == 8 && !source_wide)
			{
				i386_emit_zero_edx(gen);
			}
		} else if(!strcmp(expression->op, "&"))
		{
			i386_gen_address(gen, expression->left);
		} else if(!strcmp(expression->op, "*"))
		{
			i386_gen_expression(gen, expression->left);
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xc1);
			type = i386_expr_type(gen, expression);
			i386_emit_load_ecx_address(gen, type);
		} else {
			i386_gen_expression(gen, expression->left);
			if(!strcmp(expression->op, "!")) {
				i386_put8(&gen->text, 0x85);
				i386_put8(&gen->text, 0xc0);
				i386_emit_setcc(gen, 0x94);
			} else if(!strcmp(expression->op, "~"))
			{
				i386_put8(&gen->text, 0xf7);
				i386_put8(&gen->text, 0xd0);
			} else if(!strcmp(expression->op, "-"))
			{
				i386_put8(&gen->text, 0xf7);
				i386_put8(&gen->text, 0xd8);
			}
		}
		return;
	case EX_INDEX:
	case EX_MEMBER:
	case EX_PTRMEMBER:
		type = i386_gen_address(gen, expression);
		i386_put8(&gen->text, 0x89);
		i386_put8(&gen->text, 0xc1);
		i386_emit_load_ecx_address(gen, type);
		return;
	case EX_BINARY:
		i386_gen_binary(gen, expression);
		return;
	case EX_CALL:
		i386_gen_call(gen, expression);
		return;
	case EX_INITLIST:
		fatal("initializer list used as an expression");
		return;
	case EX_MATCH:
		fatal("match expressions are not supported in -m32 mode");
		return;
	}
	fatal("unsupported i386 expression");
}

static long i386_collect_locals(I386Gen *gen, Stmt *statement, long used)
{
	size_t index;
	Decl *declaration;
	long size;
	long alignment;
	if(!statement)
		return used;
	if(statement->kind == ST_DECL) {
		declaration = statement->decl;
		if(declaration->is_var) {
			if(!declaration->init)
				fatal("var declaration requires an initializer");
			declaration->type = i386_expr_type(gen, declaration->init);
			if(!declaration->type)
				fatal("cannot infer type of var %s", declaration->name);
		}
		if(i386_find_local(gen, declaration->name))
			fatal("duplicate local %s", declaration->name);
		size = i386_type_size(declaration->type);
		if(size <= 0)
			size = 1;
		alignment = i386_type_align(declaration->type);
		used = align_up(used, alignment);
		used += size;
		ARR_GROW(gen->locals, gen->nlocals, gen->caplocals, I386Local);
		gen->locals[gen->nlocals].name = declaration->name;
		gen->locals[gen->nlocals].type = declaration->type;
		gen->locals[gen->nlocals].offset = -used;
		gen->nlocals++;
	} else if(statement->kind == ST_BLOCK)
	{
		for(index = 0; index < statement->nchildren; index++)
			used = i386_collect_locals(gen,
						   statement->children[index],
						   used);
	} else if(statement->kind == ST_IF)
	{
		used = i386_collect_locals(gen, statement->yes, used);
		used = i386_collect_locals(gen, statement->no, used);
	} else if(statement->kind == ST_WHILE)
	{
		used = i386_collect_locals(gen, statement->body, used);
	} else if(statement->kind == ST_FOR)
	{
		used = i386_collect_locals(gen, statement->init, used);
		used = i386_collect_locals(gen, statement->body, used);
	} else if(statement->kind == ST_SWITCH)
	{
		used = i386_collect_locals(gen, statement->body, used);
	}
	return used;
}

static void i386_gen_statement(I386Gen *gen, Stmt *statement)
{
	size_t index;
	I386Local *local;
	char *else_label;
	char *done_label;
	char *loop_label;
	char *condition_label;
	char *next_label;
	if(!statement)
		return;
	switch(statement->kind) {
	case ST_BLOCK:
		for(index = 0; index < statement->nchildren; index++)
			i386_gen_statement(gen, statement->children[index]);
		return;
	case ST_DECL:
		local = i386_find_local(gen, statement->decl->name);
		if(is_vla(local->type)) {
			long element_size = i386_type_size(local->type->base);
			i386_emit_vla_bound(gen, local->type->name);
			if(element_size != 1) {
				i386_put8(&gen->text, 0x69);
				i386_put8(&gen->text, 0xc0);
				i386_put32(&gen->text, (uint32_t)element_size);
			}
			//Round the allocation up to 16 bytes.
			i386_put8(&gen->text, 0x83);
			i386_put8(&gen->text, 0xc0);
			i386_put8(&gen->text, 0x0f);
			i386_put8(&gen->text, 0x83);
			i386_put8(&gen->text, 0xe0);
			i386_put8(&gen->text, 0xf0);
			i386_put8(&gen->text, 0x29);
			i386_put8(&gen->text, 0xc4);
			//Save the runtime array base in its fixed frame slot.
			i386_put8(&gen->text, 0x89);
			i386_put8(&gen->text, 0xa5);
			i386_put32(&gen->text,
				   (uint32_t)(int32_t)local->offset);
		}
		if(statement->decl->init) {
			if(is_vla(local->type))
				fatal("variable-length array %s cannot have an initializer",
				      statement->decl->name);
			i386_emit_lea_ebp(gen, local->offset);
			i386_put8(&gen->text, 0x50);
			i386_gen_expression(gen, statement->decl->init);
			i386_put8(&gen->text, 0x59);
			i386_emit_store_ecx(gen, local->type);
		}
		return;
	case ST_EXPR:
		i386_gen_expression(gen, statement->expr);
		return;
	case ST_EMPTY:
		return;
	case ST_RETURN:
		if(statement->expr)
			i386_gen_expression(gen, statement->expr);
		else
			i386_put8(&gen->text, 0x31),
				i386_put8(&gen->text, 0xc0);
		i386_emit_jump(gen, 0xff, gen->return_label);
		return;
	case ST_IF:
		else_label = i386_new_label(gen, ".Lelse");
		done_label = i386_new_label(gen, ".Lifend");
		i386_gen_expression(gen, statement->cond);
		i386_put8(&gen->text, 0x85);
		i386_put8(&gen->text, 0xc0);
		i386_emit_jump(gen, 0x84, else_label);
		i386_gen_statement(gen, statement->yes);
		i386_emit_jump(gen, 0xff, done_label);
		i386_define_label(gen, else_label);
		i386_gen_statement(gen, statement->no);
		i386_define_label(gen, done_label);
		return;
	case ST_WHILE:
		loop_label = i386_new_label(gen, ".Lwhile");
		done_label = i386_new_label(gen, ".Lwend");
		ARR_GROW(gen->break_labels, gen->nbreak_labels, gen->capbreak_labels, char *);
		gen->break_labels[gen->nbreak_labels++] = done_label;
		ARR_GROW(gen->continue_labels, gen->ncontinue_labels, gen->capcontinue_labels, char *);
		gen->continue_labels[gen->ncontinue_labels++] = loop_label;
		i386_define_label(gen, loop_label);
		i386_gen_expression(gen, statement->cond);
		i386_put8(&gen->text, 0x85);
		i386_put8(&gen->text, 0xc0);
		i386_emit_jump(gen, 0x84, done_label);
		i386_gen_statement(gen, statement->body);
		i386_emit_jump(gen, 0xff, loop_label);
		i386_define_label(gen, done_label);
		gen->nbreak_labels--;
		gen->ncontinue_labels--;
		return;
	case ST_FOR:
		condition_label = i386_new_label(gen, ".Lforcond");
		next_label = i386_new_label(gen, ".Lfornext");
		done_label = i386_new_label(gen, ".Lforend");
		i386_gen_statement(gen, statement->init);
		ARR_GROW(gen->break_labels, gen->nbreak_labels, gen->capbreak_labels, char *);
		gen->break_labels[gen->nbreak_labels++] = done_label;
		ARR_GROW(gen->continue_labels, gen->ncontinue_labels, gen->capcontinue_labels, char *);
		gen->continue_labels[gen->ncontinue_labels++] = next_label;
		i386_define_label(gen, condition_label);
		if(statement->cond) {
			i386_gen_expression(gen, statement->cond);
			i386_put8(&gen->text, 0x85);
			i386_put8(&gen->text, 0xc0);
			i386_emit_jump(gen, 0x84, done_label);
		}
		i386_gen_statement(gen, statement->body);
		i386_define_label(gen, next_label);
		if(statement->post)
			i386_gen_expression(gen, statement->post);
		i386_emit_jump(gen, 0xff, condition_label);
		i386_define_label(gen, done_label);
		gen->nbreak_labels--;
		gen->ncontinue_labels--;
		return;
	case ST_MATCH:
		fatal("match statements are not supported in -m32 mode");
		return;
	case ST_SWITCH:
	{
		Stmt *body = statement->body;
		char *default_label;
		size_t inner_index;
		done_label = i386_new_label(gen, ".Lswitchend");
		default_label = done_label;
		if(body && body->kind == ST_BLOCK) {
			for(inner_index = 0; inner_index < body->nchildren; inner_index++) {
				Stmt *child = body->children[inner_index];
				if(child->kind == ST_CASE) {
					long value;
					if(!eval_const_expr(child->expr, &value))
						fatal("case value is not an integer constant");
					child->label = i386_new_label(gen, ".Lcase");
				} else if(child->kind == ST_DEFAULT)
				{
					if(default_label != done_label)
						fatal("multiple default labels in switch");
					child->label = i386_new_label(gen, ".Ldefault");
					default_label = child->label;
				}
			}
		} else {
			fatal("switch body must be a block");
		}
		i386_gen_expression(gen, statement->cond);
		for(inner_index = 0; inner_index < body->nchildren; inner_index++) {
			Stmt *child = body->children[inner_index];
			if(child->kind == ST_CASE) {
				long value;
				eval_const_expr(child->expr, &value);
				i386_put8(&gen->text, 0x3d);
				i386_put32(&gen->text, (uint32_t)value);
				i386_emit_jump(gen, 0x84, child->label);
			}
		}
		i386_emit_jump(gen, 0xff, default_label);
		ARR_GROW(gen->break_labels, gen->nbreak_labels, gen->capbreak_labels, char *);
		gen->break_labels[gen->nbreak_labels++] = done_label;
		i386_gen_statement(gen, body);
		gen->nbreak_labels--;
		i386_define_label(gen, done_label);
		return;
	}
	case ST_CASE:
		if(!statement->label)
			fatal("case label outside switch");
		i386_define_label(gen, statement->label);
		return;
	case ST_DEFAULT:
		if(!statement->label)
			fatal("default label outside switch");
		i386_define_label(gen, statement->label);
		return;
	case ST_BREAK:
		if(!gen->nbreak_labels)
			fatal("break outside loop or switch");
		i386_emit_jump(gen, 0xff, gen->break_labels[gen->nbreak_labels - 1]);
		return;
	case ST_CONTINUE:
		if(!gen->ncontinue_labels)
			fatal("continue outside loop");
		i386_emit_jump(gen, 0xff, gen->continue_labels[gen->ncontinue_labels - 1]);
		return;
	case ST_ASM:
	{
		if(!strcmp(statement->asm_text, "rdtsc")) {
			size_t output_index;
			i386_put8(&gen->text, 0x0f);
			i386_put8(&gen->text, 0x31);
			for(output_index = 0; output_index < statement->nasm_outputs;
			    output_index++)
			{
				const char *constraint =
					statement->asm_constraints[output_index];
				Expr *output = statement->asm_outputs[output_index];
				CType *output_type;
				if(!strcmp(constraint, "=a")) {
					//Preserve EDX:EAX while calculating the lvalue.
					i386_put8(&gen->text, 0x52);
					i386_put8(&gen->text, 0x50);
					output_type = i386_gen_address(gen, output);
					i386_put8(&gen->text, 0x89);
					i386_put8(&gen->text, 0xc1);
					i386_put8(&gen->text, 0x58);
					i386_emit_store_ecx(gen, output_type);
					i386_put8(&gen->text, 0x5a);
				} else if(!strcmp(constraint, "=d"))
				{
					//Store the high half while restoring low EAX.
					i386_put8(&gen->text, 0x50);
					i386_put8(&gen->text, 0x52);
					output_type = i386_gen_address(gen, output);
					i386_put8(&gen->text, 0x89);
					i386_put8(&gen->text, 0xc1);
					i386_put8(&gen->text, 0x58);
					i386_emit_store_ecx(gen, output_type);
					i386_put8(&gen->text, 0x58);
				} else {
					fatal("unsupported rdtsc output constraint %s",
					      constraint);
				}
			}
			return;
		}
		char *copy = xstrdup(statement->asm_text);
		char *cursor = copy;
		while(cursor && *cursor) {
			char *next = strchr(cursor, ';');
			char *end;
			if(next)
				*next++ = 0;
			while(isspace((unsigned char)*cursor))
				cursor++;
			end = cursor + strlen(cursor);
			while(end > cursor && isspace((unsigned char)end[-1]))
				*--end = 0;
			if(!*cursor) {
				cursor = next;
				continue;
			}
			if(!strcmp(cursor, "hlt"))
				i386_put8(&gen->text, 0xf4);
			else if(!strcmp(cursor, "cli"))
				i386_put8(&gen->text, 0xfa);
			else if(!strcmp(cursor, "sti"))
				i386_put8(&gen->text, 0xfb);
			else if(!strcmp(cursor, "nop"))
				i386_put8(&gen->text, 0x90);
			else if(!strcmp(cursor, "cld"))
				i386_put8(&gen->text, 0xfc);
			else if(!strcmp(cursor, "std"))
				i386_put8(&gen->text, 0xfd);
			else if(!strcmp(cursor, "int3"))
				i386_put8(&gen->text, 0xcc);
			else if(!strcmp(cursor, "pause")) {
				i386_put8(&gen->text, 0xf3);
				i386_put8(&gen->text, 0x90);
			} else if(!strcmp(cursor, "ud2"))
			{
				i386_put8(&gen->text, 0x0f);
				i386_put8(&gen->text, 0x0b);
			} else {
				char *bad = xstrdup(cursor);
				free(copy);
				fatal("unsupported inline asm instruction: %s", bad);
			}
			cursor = next;
		}
		free(copy);
		return;
	}
	}
}

static void i386_gen_function(I386Gen *gen, Decl *declaration)
{
	I386ObjSymbol *symbol;
	long used = 0;
	size_t index;
	uint32_t start;
	gen->current = declaration;
	gen->nlocals = 0;
	for(index = 0; index < declaration->nparams; index++) {
		ARR_GROW(gen->locals, gen->nlocals, gen->caplocals, I386Local);
		gen->locals[gen->nlocals].name = declaration->params[index].name;
		gen->locals[gen->nlocals].type = declaration->params[index].type;
		gen->locals[gen->nlocals].offset = 8 + (long)index * 4;
		gen->nlocals++;
	}
	used = i386_collect_locals(gen, declaration->body, used);
	gen->frame_size = align_up(used, 4);
	snprintf(gen->return_label, sizeof(gen->return_label), ".Lreturn_%s_%ld", declaration->name, ++gen->label_number);
	start = (uint32_t)gen->text.n;
	symbol = i386_add_object_symbol(gen, declaration->name, declaration->type, true);
	symbol->local = declaration->is_static;
	symbol->defined = true;
	symbol->section = I386_SEC_TEXT;
	symbol->value = start;
	i386_put8(&gen->text, 0x55);
	i386_put8(&gen->text, 0x89);
	i386_put8(&gen->text, 0xe5);
	if(gen->frame_size) {
		i386_put8(&gen->text, 0x81);
		i386_put8(&gen->text, 0xec);
		i386_put32(&gen->text, (uint32_t)gen->frame_size);
	}
	i386_gen_statement(gen, declaration->body);
	i386_put8(&gen->text, 0x31);
	i386_put8(&gen->text, 0xc0);
	i386_define_label(gen, gen->return_label);
	i386_put8(&gen->text, 0xc9);
	i386_put8(&gen->text, 0xc3);
	symbol->size = (uint32_t)gen->text.n - start;
}
static uint32_t i386_intern_string(I386Gen *gen, const char *value);

static void i386_put_zeros(Buf *buffer, size_t count)
{
	while(count--)
		i386_put8(buffer, 0);
}

static void i386_write_global_initializer(I386Gen *gen, CType *type, Expr *initializer, const char *name)
{
	size_t start = gen->data.n;
	long size = i386_type_size(type);
	if(type->kind == TY_ARRAY) {
		size_t index_1;
		if(initializer->kind == EX_STR &&
		   (type->base->kind == TY_CHAR || type->base->kind == TY_U8))
		{
			size_t bytes = strlen(initializer->str) + 1;
			if(bytes > (size_t)type->count)
				fatal("initializer string is too long for %s", name);
			bputn(&gen->data, initializer->str, bytes);
			i386_put_zeros(&gen->data, (size_t)type->count - bytes);
			return;
		}
		if(initializer->kind != EX_INITLIST)
			fatal("array initializer for %s must use braces", name);
		if(initializer->nargs > (size_t)type->count)
			fatal("too many initializers for %s", name);
		for(index_1 = 0; index_1 < initializer->nargs; index_1++)
			i386_write_global_initializer(gen, type->base, initializer->args[index_1], name);
		i386_put_zeros(&gen->data,
			       (size_t)size - (gen->data.n - start));
		return;
	}
	if(type->kind == TY_STRUCT) {
		size_t index_1;
		if(initializer->kind != EX_INITLIST)
			fatal("struct initializer for %s must use braces", name);
		if(initializer->nargs > type->nmembers)
			fatal("too many initializers for %s", name);
		for(index_1 = 0; index_1 < initializer->nargs; index_1++) {
			long member_offset = 0;
			if(!i386_find_struct_member(type, type->members[index_1].name, &member_offset))
				fatal("internal: bad member offset in %s", name);
			if(gen->data.n < start + (size_t)member_offset)
				i386_put_zeros(&gen->data,
					       start + (size_t)member_offset - gen->data.n);
			i386_write_global_initializer(gen, type->members[index_1].type, initializer->args[index_1], name);
		}
		if(gen->data.n < start + (size_t)size)
			i386_put_zeros(&gen->data, start + (size_t)size - gen->data.n);
		return;
	}
	if(initializer->kind == EX_INITLIST) {
		if(initializer->nargs != 1)
			fatal("scalar initializer for %s has %zu elements", name, initializer->nargs);
		initializer = initializer->args[0];
	}
	if(initializer->kind == EX_STR && type->kind == TY_PTR) {
		uint32_t string_offset = i386_intern_string(gen, initializer->str);
		uint32_t relocation_offset = (uint32_t)gen->data.n;
		i386_put32(&gen->data, string_offset);
		i386_add_relocation(gen, I386_SEC_DATA, relocation_offset, R_386_32, NULL, I386_SEC_RODATA);
		return;
	}
	{
		long constant_value;
		uint64_t value;
		if(!eval_const_expr(initializer, &constant_value))
			fatal("unsupported i386 global initializer for %s", name);
		value = (uint64_t)(unsigned long)constant_value;
		if(size == 1)
			i386_put8(&gen->data, (uint8_t)value);
		else if(size == 2)
			i386_put16(&gen->data, (uint16_t)value);
		else if(size == 4)
			i386_put32(&gen->data, (uint32_t)value);
		else if(size == 8)
			bputn(&gen->data, (char *)&value, 8);
		else
			fatal("unsupported i386 scalar size for %s", name);
	}
}

static void i386_prepare_symbols_and_globals(I386Gen *gen)
{
	size_t index;
	for(index = 0; index < gen->program->n; index++) {
		Decl *declaration = gen->program->a[index];
		I386ObjSymbol *symbol;
		if(declaration->body || declaration->prototype) {
			symbol = i386_add_object_symbol(gen, declaration->name, declaration->type, true);
			if(declaration->is_static)
				symbol->local = true;
			if(declaration->body)
				symbol->defined = true;
			continue;
		}
		if(is_vla(declaration->type))
			fatal("variable-length array %s is only supported at block scope",
			      declaration->name);
		symbol = i386_add_object_symbol(gen, declaration->name, declaration->type, false);
		if(declaration->is_static)
			symbol->local = true;
		if(declaration->is_extern)
			continue;
		if(declaration->init) {
			long size = i386_type_size(declaration->type);
			while(gen->data.n % (size_t)i386_type_align(
						    declaration->type))
				i386_put8(&gen->data, 0);
			symbol->defined = true;
			symbol->section = I386_SEC_DATA;
			symbol->value = (uint32_t)gen->data.n;
			symbol->size = (uint32_t)size;
			i386_write_global_initializer(gen, declaration->type, declaration->init, declaration->name);
		} else {
			long alignment = i386_type_align(declaration->type);
			long size = i386_type_size(declaration->type);
			gen->bss_size = i386_align32(gen->bss_size,
						     (uint32_t)alignment);
			symbol->defined = true;
			symbol->section = I386_SEC_BSS;
			symbol->value = gen->bss_size;
			symbol->size = (uint32_t)(size > 0 ? size : 1);
			gen->bss_size += symbol->size;
		}
	}
}

static uint32_t i386_string_offset(Buf *strings, const char *string)
{
	uint32_t offset = (uint32_t)strings->n;
	bputn(strings, string, strlen(string) + 1);
	return offset;
}

static uint32_t i386_symbol_index(I386Gen *gen, const char *name)
{
	size_t index_1;
	for(index_1 = 0; index_1 < gen->nsymbols; index_1++)
		if(!strcmp(gen->symbols[index_1].name, name))
			return gen->symbols[index_1].index;
	fatal("internal: relocation references unknown symbol %s", name);
	return 0;
}

void write_i386_relocatable(const char *path, Program *program)
{
	I386Gen gen = {0};
	Buf strtab = {0};
	Buf shstrtab = {0};
	Elf32_Sym *symtab;
	Elf32_Rel *text_relocations;
	Elf32_Rel *data_relocations;
	Elf32_Shdr sections[I386_SEC_COUNT];
	Elf32_Ehdr header;
	uint32_t section_names[I386_SEC_COUNT];
	uint32_t cursor;
	uint32_t text_offset;
	uint32_t rodata_offset;
	uint32_t data_offset;
	uint32_t rel_text_offset;
	uint32_t rel_data_offset;
	uint32_t symtab_offset;
	uint32_t strtab_offset;
	uint32_t shstrtab_offset;
	uint32_t section_headers_offset;
	uint32_t first_global;
	uint32_t local_symbol_count = 0;
	uint32_t symbol_count;
	uint32_t text_relocation_count = 0;
	uint32_t data_relocation_count = 0;
	uint8_t *file;
	size_t index_1;
	gen.program = program;
	i386_prepare_symbols_and_globals(&gen);
	for(index_1 = 0; index_1 < program->n; index_1++)
		if(program->a[index_1]->body)
			i386_gen_function(&gen, program->a[index_1]);
	i386_patch_jumps(&gen);
	put_u8(&strtab, 0);
	put_u8(&shstrtab, 0);
	memset(section_names, 0, sizeof(section_names));
	section_names[I386_SEC_TEXT] = i386_string_offset(&shstrtab,
							  ".text");
	section_names[I386_SEC_RODATA] = i386_string_offset(&shstrtab,
							    ".rodata");
	section_names[I386_SEC_DATA] = i386_string_offset(&shstrtab,
							  ".data");
	section_names[I386_SEC_BSS] = i386_string_offset(&shstrtab,
							 ".bss");
	section_names[I386_SEC_REL_TEXT] = i386_string_offset(&shstrtab,
							      ".rel.text");
	section_names[I386_SEC_REL_DATA] = i386_string_offset(&shstrtab,
							      ".rel.data");
	section_names[I386_SEC_SYMTAB] = i386_string_offset(&shstrtab,
							    ".symtab");
	section_names[I386_SEC_STRTAB] = i386_string_offset(&shstrtab,
							    ".strtab");
	section_names[I386_SEC_SHSTRTAB] = i386_string_offset(&shstrtab,
							      ".shstrtab");
	for(index_1 = 0; index_1 < gen.nsymbols; index_1++)
		if(gen.symbols[index_1].local)
			local_symbol_count++;
	first_global = 5 + local_symbol_count;
	symbol_count = 5 + (uint32_t)gen.nsymbols;
	symtab = xcalloc(symbol_count, sizeof(*symtab));
	symtab[1].st_info = ELF32_ST_INFO(STB_LOCAL, STT_SECTION);
	symtab[1].st_shndx = I386_SEC_TEXT;
	symtab[2].st_info = ELF32_ST_INFO(STB_LOCAL, STT_SECTION);
	symtab[2].st_shndx = I386_SEC_RODATA;
	symtab[3].st_info = ELF32_ST_INFO(STB_LOCAL, STT_SECTION);
	symtab[3].st_shndx = I386_SEC_DATA;
	symtab[4].st_info = ELF32_ST_INFO(STB_LOCAL, STT_SECTION);
	symtab[4].st_shndx = I386_SEC_BSS;
	{
		uint32_t local_index = 5;
		uint32_t global_index = first_global;
		for(index_1 = 0; index_1 < gen.nsymbols; index_1++) {
			I386ObjSymbol *object = &gen.symbols[index_1];
			Elf32_Sym *symbol;
			object->index = object->local ? local_index++ : global_index++;
			symbol = &symtab[object->index];
			symbol->st_name = i386_string_offset(&strtab, object->name);
			symbol->st_value = object->value;
			symbol->st_size = object->size;
			symbol->st_info = ELF32_ST_INFO(
				object->local ? STB_LOCAL : STB_GLOBAL,
				object->function ? STT_FUNC : STT_OBJECT);
			symbol->st_other = STV_DEFAULT;
			symbol->st_shndx = object->defined ? object->section : SHN_UNDEF;
		}
	}
	for(index_1 = 0; index_1 < gen.nrelocations; index_1++) {
		if(gen.relocations[index_1].target_section == I386_SEC_TEXT)
			text_relocation_count++;
		else if(gen.relocations[index_1].target_section == I386_SEC_DATA)
			data_relocation_count++;
		else
			fatal("internal: unsupported i386 relocation target section");
	}
	text_relocations = xcalloc(text_relocation_count ? text_relocation_count : 1, sizeof(*text_relocations));
	data_relocations = xcalloc(data_relocation_count ? data_relocation_count : 1, sizeof(*data_relocations));
	text_relocation_count = 0;
	data_relocation_count = 0;
	for(index_1 = 0; index_1 < gen.nrelocations; index_1++) {
		I386Relocation *source = &gen.relocations[index_1];
		Elf32_Rel *destination;
		uint32_t symbol_index;
		if(source->section_symbol) {
			switch(source->section_symbol) {
			case I386_SEC_TEXT:
				symbol_index = 1;
				break;
			case I386_SEC_RODATA:
				symbol_index = 2;
				break;
			case I386_SEC_DATA:
				symbol_index = 3;
				break;
			case I386_SEC_BSS:
				symbol_index = 4;
				break;
			default:
				fatal("internal: bad i386 section relocation");
			}
		} else {
			symbol_index = i386_symbol_index(&gen, source->symbol);
		}
		if(source->target_section == I386_SEC_TEXT)
			destination = &text_relocations[text_relocation_count++];
		else
			destination = &data_relocations[data_relocation_count++];
		destination->r_offset = source->offset;
		destination->r_info = ELF32_R_INFO(symbol_index,
						   source->type);
	}
	/*
		PREPARE YOUR EYES FOR THIS GIGANTIC CHUNK!
	*/
	cursor = sizeof(Elf32_Ehdr);
	text_offset = i386_align32(cursor, 16);
	cursor = text_offset + (uint32_t)gen.text.n;
	rodata_offset = i386_align32(cursor, 4);
	cursor = rodata_offset + (uint32_t)gen.rodata.n;
	data_offset = i386_align32(cursor, 4);
	cursor = data_offset + (uint32_t)gen.data.n;
	rel_text_offset = i386_align32(cursor, 4);
	cursor = rel_text_offset +
		 text_relocation_count * sizeof(Elf32_Rel);
	rel_data_offset = i386_align32(cursor, 4);
	cursor = rel_data_offset +
		 data_relocation_count * sizeof(Elf32_Rel);
	symtab_offset = i386_align32(cursor, 4);
	cursor = symtab_offset + symbol_count * sizeof(Elf32_Sym);
	strtab_offset = cursor;
	cursor += (uint32_t)strtab.n;
	shstrtab_offset = cursor;
	cursor += (uint32_t)shstrtab.n;
	section_headers_offset = i386_align32(cursor, 4);
	cursor = section_headers_offset +
		 I386_SEC_COUNT * sizeof(Elf32_Shdr);
	file = xcalloc(cursor, 1);
	memset(&header, 0, sizeof(header));
	memcpy(header.e_ident, ELFMAG, SELFMAG);
	header.e_ident[EI_CLASS] = ELFCLASS32;
	header.e_ident[EI_DATA] = ELFDATA2LSB;
	header.e_ident[EI_VERSION] = EV_CURRENT;
	header.e_ident[EI_OSABI] = ELFOSABI_SYSV;
	header.e_type = ET_REL;
	header.e_machine = EM_386;
	header.e_version = EV_CURRENT;
	header.e_ehsize = sizeof(Elf32_Ehdr);
	header.e_shoff = section_headers_offset;
	header.e_shentsize = sizeof(Elf32_Shdr);
	header.e_shnum = I386_SEC_COUNT;
	header.e_shstrndx = I386_SEC_SHSTRTAB;
	memset(sections, 0, sizeof(sections));
	sections[I386_SEC_TEXT].sh_name = section_names[I386_SEC_TEXT];
	sections[I386_SEC_TEXT].sh_type = SHT_PROGBITS;
	sections[I386_SEC_TEXT].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
	sections[I386_SEC_TEXT].sh_offset = text_offset;
	sections[I386_SEC_TEXT].sh_size = gen.text.n;
	sections[I386_SEC_TEXT].sh_addralign = 16;
	sections[I386_SEC_RODATA].sh_name = section_names[I386_SEC_RODATA];
	sections[I386_SEC_RODATA].sh_type = SHT_PROGBITS;
	sections[I386_SEC_RODATA].sh_flags = SHF_ALLOC;
	sections[I386_SEC_RODATA].sh_offset = rodata_offset;
	sections[I386_SEC_RODATA].sh_size = gen.rodata.n;
	sections[I386_SEC_RODATA].sh_addralign = 1;
	sections[I386_SEC_DATA].sh_name = section_names[I386_SEC_DATA];
	sections[I386_SEC_DATA].sh_type = SHT_PROGBITS;
	sections[I386_SEC_DATA].sh_flags = SHF_ALLOC | SHF_WRITE;
	sections[I386_SEC_DATA].sh_offset = data_offset;
	sections[I386_SEC_DATA].sh_size = gen.data.n;
	sections[I386_SEC_DATA].sh_addralign = 4;
	sections[I386_SEC_BSS].sh_name = section_names[I386_SEC_BSS];
	sections[I386_SEC_BSS].sh_type = SHT_NOBITS;
	sections[I386_SEC_BSS].sh_flags = SHF_ALLOC | SHF_WRITE;
	sections[I386_SEC_BSS].sh_offset = data_offset + gen.data.n;
	sections[I386_SEC_BSS].sh_size = gen.bss_size;
	sections[I386_SEC_BSS].sh_addralign = 4;
	sections[I386_SEC_REL_TEXT].sh_name =
		section_names[I386_SEC_REL_TEXT];
	sections[I386_SEC_REL_TEXT].sh_type = SHT_REL;
	sections[I386_SEC_REL_TEXT].sh_offset = rel_text_offset;
	sections[I386_SEC_REL_TEXT].sh_size =
		text_relocation_count * sizeof(Elf32_Rel);
	sections[I386_SEC_REL_TEXT].sh_link = I386_SEC_SYMTAB;
	sections[I386_SEC_REL_TEXT].sh_info = I386_SEC_TEXT;
	sections[I386_SEC_REL_TEXT].sh_addralign = 4;
	sections[I386_SEC_REL_TEXT].sh_entsize = sizeof(Elf32_Rel);
	sections[I386_SEC_REL_DATA].sh_name =
		section_names[I386_SEC_REL_DATA];
	sections[I386_SEC_REL_DATA].sh_type = SHT_REL;
	sections[I386_SEC_REL_DATA].sh_offset = rel_data_offset;
	sections[I386_SEC_REL_DATA].sh_size =
		data_relocation_count * sizeof(Elf32_Rel);
	sections[I386_SEC_REL_DATA].sh_link = I386_SEC_SYMTAB;
	sections[I386_SEC_REL_DATA].sh_info = I386_SEC_DATA;
	sections[I386_SEC_REL_DATA].sh_addralign = 4;
	sections[I386_SEC_REL_DATA].sh_entsize = sizeof(Elf32_Rel);
	sections[I386_SEC_SYMTAB].sh_name = section_names[I386_SEC_SYMTAB];
	sections[I386_SEC_SYMTAB].sh_type = SHT_SYMTAB;
	sections[I386_SEC_SYMTAB].sh_offset = symtab_offset;
	sections[I386_SEC_SYMTAB].sh_size = symbol_count * sizeof(Elf32_Sym);
	sections[I386_SEC_SYMTAB].sh_link = I386_SEC_STRTAB;
	sections[I386_SEC_SYMTAB].sh_info = first_global;
	sections[I386_SEC_SYMTAB].sh_addralign = 4;
	sections[I386_SEC_SYMTAB].sh_entsize = sizeof(Elf32_Sym);
	sections[I386_SEC_STRTAB].sh_name = section_names[I386_SEC_STRTAB];
	sections[I386_SEC_STRTAB].sh_type = SHT_STRTAB;
	sections[I386_SEC_STRTAB].sh_offset = strtab_offset;
	sections[I386_SEC_STRTAB].sh_size = strtab.n;
	sections[I386_SEC_STRTAB].sh_addralign = 1;
	sections[I386_SEC_SHSTRTAB].sh_name =
		section_names[I386_SEC_SHSTRTAB];
	sections[I386_SEC_SHSTRTAB].sh_type = SHT_STRTAB;
	sections[I386_SEC_SHSTRTAB].sh_offset = shstrtab_offset;
	sections[I386_SEC_SHSTRTAB].sh_size = shstrtab.n;
	sections[I386_SEC_SHSTRTAB].sh_addralign = 1;
	memcpy(file, &header, sizeof(header));
	if(gen.text.n)
		memcpy(file + text_offset, gen.text.s, gen.text.n);
	if(gen.rodata.n)
		memcpy(file + rodata_offset, gen.rodata.s, gen.rodata.n);
	if(gen.data.n)
		memcpy(file + data_offset, gen.data.s, gen.data.n);
	if(text_relocation_count)
		memcpy(file + rel_text_offset, text_relocations, text_relocation_count * sizeof(Elf32_Rel));
	if(data_relocation_count)
		memcpy(file + rel_data_offset, data_relocations, data_relocation_count * sizeof(Elf32_Rel));
	memcpy(file + symtab_offset, symtab, symbol_count * sizeof(Elf32_Sym));
	memcpy(file + strtab_offset, strtab.s, strtab.n);
	memcpy(file + shstrtab_offset, shstrtab.s, shstrtab.n);
	memcpy(file + section_headers_offset, sections, sizeof(sections));
	write_file(path, (char *)file, cursor);
}

