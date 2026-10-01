#include "assembler.h"

static uint64_t ualign(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

void put_u8(Buf *buffer, uint8_t value)
{
	bputn(buffer, (char *)&value, 1);
}

static void put_u32(Buf *buffer, uint32_t value)
{
	uint8_t bytes[4];
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
	bputn(buffer, (char *)bytes, sizeof(bytes));
}

static void put_u64(Buf *buffer, uint64_t value)
{
	uint8_t bytes[8];
	int index;
	for(index = 0; index < 8; index++)
		bytes[index] = (uint8_t)(value >> (index * 8));
	bputn(buffer, (char *)bytes, sizeof(bytes));
}

static void patch_u32(Buf *buffer, uint64_t offset, uint32_t value)
{
	if(offset + 4 > buffer->n)
		fatal("internal: patch outside code buffer");
	buffer->s[offset + 0] = (char)value;
	buffer->s[offset + 1] = (char)(value >> 8);
	buffer->s[offset + 2] = (char)(value >> 16);
	buffer->s[offset + 3] = (char)(value >> 24);
}

static Buf *current_buffer(AsmImage *array)
{
	if(array->section == ASEC_TEXT)
		return &array->text;
	if(array->section == ASEC_RODATA)
		return &array->rodata;
	fatal("internal: attempted to emit bytes into BSS");
	return NULL;
}

static uint64_t current_offset(AsmImage *array)
{
	if(array->section == ASEC_TEXT)
		return array->text.n;
	if(array->section == ASEC_RODATA)
		return array->rodata.n;
	return array->bss_size;
}

static void add_label(AsmImage *array, const char *name)
{
	size_t index;
	for(index = 0; index < array->nlabels; index++)
		if(!strcmp(array->labels[index].name, name))
			fatal("duplicate assembly label %s", name);
	ARR_GROW(array->labels, array->nlabels, array->caplabels, ALabel);
	array->labels[array->nlabels].name = xstrdup(name);
	array->labels[array->nlabels].section = array->section;
	array->labels[array->nlabels].offset = current_offset(array);
	array->nlabels++;
}

static ALabel *find_label(AsmImage *array, const char *name)
{
	size_t index;
	for(index = 0; index < array->nlabels; index++)
		if(!strcmp(array->labels[index].name, name))
			return &array->labels[index];
	return NULL;
}

static Import *find_import(AsmImage *array, const char *name)
{
	size_t index;
	for(index = 0; index < array->nimports; index++)
		if(!strcmp(array->imports[index].name, name))
			return &array->imports[index];
	return NULL;
}

static Import *add_import(AsmImage *array, const char *name, bool object)
{
	Import *import = find_import(array, name);
	if(import) {
		if(object)
			import->object = true;
		return import;
	}
	ARR_GROW(array->imports, array->nimports, array->capimports, Import);
	import = &array->imports[array->nimports++];
	memset(import, 0, sizeof(*import));
	import->name = xstrdup(name);
	import->object = object;
	return import;
}

static void add_fixup(AsmImage *array, FixKind kind, uint64_t offset, const char *name, long aux)
{
	ARR_GROW(array->fixups, array->nfixups, array->capfixups, Fixup);
	array->fixups[array->nfixups].kind = kind;
	array->fixups[array->nfixups].offset = offset;
	array->fixups[array->nfixups].name = name ? xstrdup(name) : NULL;
	array->fixups[array->nfixups].aux = aux;
	array->nfixups++;
}

static int register_number(const char *name)
{
	static const char *names[] = {
		"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
	int index;
	for(index = 0; index < 16; index++)
		if(!strcmp(name, names[index]))
			return index;
	return -1;
}

static int xmm_number(const char *name)
{
	char *end;
	long number;
	if(strncmp(name, "xmm", 3))
		return -1;
	number = strtol(name + 3, &end, 10);
	if(*end || number < 0 || number > 15)
		return -1;
	return (int)number;
}

static void emit_rex(Buf *buffer, bool wide_operand, int reg, int index, int base)
{
	uint8_t rex = 0x40;
	if(wide_operand)
		rex |= 8;
	if(reg & 8)
		rex |= 4;
	if(index & 8)
		rex |= 2;
	if(base & 8)
		rex |= 1;
	if(rex != 0x40)
		put_u8(buffer, rex);
}

static void emit_modrm(Buf *buffer, int mod, int reg, int rm_operand)
{
	put_u8(buffer, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm_operand & 7)));
}

static void emit_reg_reg(Buf *buffer, uint8_t opcode, int destination, int source)
{
	emit_rex(buffer, true, source, 0, destination);
	put_u8(buffer, opcode);
	emit_modrm(buffer, 3, source, destination);
}

static void emit_memory_operand(Buf *buffer, int reg, int base, long displacement)
{
	int mod;
	if(displacement == 0 && (base & 7) != 5)
		mod = 0;
	else if(displacement >= -128 && displacement <= 127)
		mod = 1;
	else
		mod = 2;
	emit_modrm(buffer, mod, reg, base);
	if((base & 7) == 4)
		put_u8(buffer, 0x24);
	if(mod == 1)
		put_u8(buffer, (uint8_t)displacement);
	else if(mod == 2 || (mod == 0 && (base & 7) == 5))
		put_u32(buffer, (uint32_t)displacement);
}

static void emit_mov_memory_register(Buf *buffer, int base, long displacement, int source, int size)
{
	if(size == 8) {
		emit_rex(buffer, true, source, 0, base);
		put_u8(buffer, 0x89);
	} else if(size == 4)
	{
		emit_rex(buffer, false, source, 0, base);
		put_u8(buffer, 0x89);
	} else {
		emit_rex(buffer, false, source, 0, base);
		put_u8(buffer, 0x88);
	}
	emit_memory_operand(buffer, source, base, displacement);
}

static void emit_mov_register_memory(Buf *buffer, int destination, int base, long displacement, int size)
{
	if(size == 8) {
		emit_rex(buffer, true, destination, 0, base);
		put_u8(buffer, 0x8b);
	} else {
		emit_rex(buffer, false, destination, 0, base);
		put_u8(buffer, 0x8b);
	}
	emit_memory_operand(buffer, destination, base, displacement);
}

static void emit_lea_memory(Buf *buffer, int destination, int base, long displacement)
{
	emit_rex(buffer, true, destination, 0, base);
	put_u8(buffer, 0x8d);
	emit_memory_operand(buffer, destination, base, displacement);
}

static char *trim(char *line)
{
	char *end;
	while(*line && isspace((unsigned char)*line))
		line++;
	end = line + strlen(line);
	while(end > line && isspace((unsigned char)end[-1]))
		*--end = 0;
	return line;
}

static void emit_relative_fixup(AsmImage *array, uint8_t opcode, const char *name, bool conditional, uint8_t condition)
{
	Buf *buffer = &array->text;
	if(conditional) {
		put_u8(buffer, 0x0f);
		put_u8(buffer, condition);
	} else {
		put_u8(buffer, opcode);
	}
	add_fixup(array, FIX_REL32_SYMBOL, buffer->n, name, 0);
	put_u32(buffer, 0);
}

static void assemble_instruction(AsmImage *array, char *line)
{
	Buf *buffer = current_buffer(array);
	char left[128], right[128], name[256], reg1[32], reg2[32];
	long long signed_value;
	unsigned long long unsigned_value;
	long displacement;
	int first_register, second_register;
	if(!strcmp(line, "push rbp")) {
		put_u8(buffer, 0x55);
		return;
	}
	if(!strcmp(line, "leave")) {
		put_u8(buffer, 0xc9);
		return;
	}
	if(!strcmp(line, "ret")) {
		put_u8(buffer, 0xc3);
		return;
	}
	if(!strcmp(line, "hlt")) {
		put_u8(buffer, 0xf4);
		return;
	}
	if(!strcmp(line, "cli")) {
		put_u8(buffer, 0xfa);
		return;
	}
	if(!strcmp(line, "sti")) {
		put_u8(buffer, 0xfb);
		return;
	}
	if(!strcmp(line, "nop")) {
		put_u8(buffer, 0x90);
		return;
	}
	if(!strcmp(line, "cld")) {
		put_u8(buffer, 0xfc);
		return;
	}
	if(!strcmp(line, "std")) {
		put_u8(buffer, 0xfd);
		return;
	}
	if(!strcmp(line, "int3")) {
		put_u8(buffer, 0xcc);
		return;
	}
	if(!strcmp(line, "pause")) {
		put_u8(buffer, 0xf3);
		put_u8(buffer, 0x90);
		return;
	}
	if(!strcmp(line, "ud2")) {
		put_u8(buffer, 0x0f);
		put_u8(buffer, 0x0b);
		return;
	}
	if(!strcmp(line, "cqo")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x99);
		return;
	}
	if(!strcmp(line, "xor eax, eax")) {
		put_u8(buffer, 0x31);
		put_u8(buffer, 0xc0);
		return;
	}
	if(!strcmp(line, "test rax, rax")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x85);
		put_u8(buffer, 0xc0);
		return;
	}
	if(!strcmp(line, "cmp rcx, rax")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x39);
		put_u8(buffer, 0xc1);
		return;
	}
	if(!strcmp(line, "movzx eax, byte ptr [rax]")) {
		put_u8(buffer, 0x0f);
		put_u8(buffer, 0xb6);
		put_u8(buffer, 0x00);
		return;
	}
	if(!strcmp(line, "movzx eax, word ptr [rax]")) {
		put_u8(buffer, 0x0f);
		put_u8(buffer, 0xb7);
		put_u8(buffer, 0x00);
		return;
	}
	if(!strcmp(line, "mov eax, dword ptr [rax]")) {
		put_u8(buffer, 0x8b);
		put_u8(buffer, 0x00);
		return;
	}
	if(!strcmp(line, "movsxd rax, dword ptr [rax]")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x63);
		put_u8(buffer, 0x00);
		return;
	}
	if(!strcmp(line, "mov rax, qword ptr [rax]")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x8b);
		put_u8(buffer, 0x00);
		return;
	}
	if(!strcmp(line, "mov byte ptr [rcx], al")) {
		put_u8(buffer, 0x88);
		put_u8(buffer, 0x01);
		return;
	}
	if(!strcmp(line, "mov word ptr [rcx], ax")) {
		put_u8(buffer, 0x66);
		put_u8(buffer, 0x89);
		put_u8(buffer, 0x01);
		return;
	}
	if(!strcmp(line, "mov dword ptr [rcx], eax")) {
		put_u8(buffer, 0x89);
		put_u8(buffer, 0x01);
		return;
	}
	if(!strcmp(line, "mov qword ptr [rcx], rax")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x89);
		put_u8(buffer, 0x01);
		return;
	}
	if(!strcmp(line, "movzx rax, al")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x0f);
		put_u8(buffer, 0xb6);
		put_u8(buffer, 0xc0);
		return;
	}
	if(!strcmp(line, "movzx eax, al")) {
		put_u8(buffer, 0x0f);
		put_u8(buffer, 0xb6);
		put_u8(buffer, 0xc0);
		return;
	}
	if(!strcmp(line, "movzx eax, ax")) {
		put_u8(buffer, 0x0f);
		put_u8(buffer, 0xb7);
		put_u8(buffer, 0xc0);
		return;
	}
	if(!strcmp(line, "movsxd rax, eax")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x63);
		put_u8(buffer, 0xc0);
		return;
	}
	if(!strcmp(line, "mov eax, eax")) {
		put_u8(buffer, 0x89);
		put_u8(buffer, 0xc0);
		return;
	}
	if(sscanf(line, "movq %31[^,], %31s", reg1, reg2) == 2) {
		int destination_xmm = xmm_number(reg1);
		int source_xmm = xmm_number(reg2);
		first_register = register_number(reg1);
		second_register = register_number(reg2);
		if(destination_xmm >= 0 && second_register >= 0) {
			put_u8(buffer, 0x66);
			emit_rex(buffer, true, destination_xmm, 0, second_register);
			put_u8(buffer, 0x0f);
			put_u8(buffer, 0x6e);
			emit_modrm(buffer, 3, destination_xmm, second_register);
			return;
		}
		if(first_register >= 0 && source_xmm >= 0) {
			put_u8(buffer, 0x66);
			emit_rex(buffer, true, source_xmm, 0, first_register);
			put_u8(buffer, 0x0f);
			put_u8(buffer, 0x7e);
			emit_modrm(buffer, 3, source_xmm, first_register);
			return;
		}
	}
	if(sscanf(line, "cvtsi2sd %31[^,], %31s", reg1, reg2) == 2) {
		int destination_xmm = xmm_number(reg1);
		second_register = register_number(reg2);
		if(destination_xmm >= 0 && second_register >= 0) {
			put_u8(buffer, 0xf2);
			emit_rex(buffer, true, destination_xmm, 0, second_register);
			put_u8(buffer, 0x0f);
			put_u8(buffer, 0x2a);
			emit_modrm(buffer, 3, destination_xmm, second_register);
			return;
		}
	}
	if(sscanf(line, "cvttsd2si %31[^,], %31s", reg1, reg2) == 2) {
		first_register = register_number(reg1);
		int source_xmm = xmm_number(reg2);
		if(first_register >= 0 && source_xmm >= 0) {
			put_u8(buffer, 0xf2);
			emit_rex(buffer, true, first_register, 0, source_xmm);
			put_u8(buffer, 0x0f);
			put_u8(buffer, 0x2c);
			emit_modrm(buffer, 3, first_register, source_xmm);
			return;
		}
	}
	{
		char mnemonic[16];
		if(sscanf(line, "%15s %31[^,], %31s", mnemonic, reg1, reg2) == 3) {
			int destination_xmm = xmm_number(reg1);
			int source_xmm = xmm_number(reg2);
			uint8_t opcode = 0;
			if(!strcmp(mnemonic, "addsd"))
				opcode = 0x58;
			else if(!strcmp(mnemonic, "subsd"))
				opcode = 0x5c;
			else if(!strcmp(mnemonic, "mulsd"))
				opcode = 0x59;
			else if(!strcmp(mnemonic, "divsd"))
				opcode = 0x5e;
			if(opcode && destination_xmm >= 0 && source_xmm >= 0) {
				put_u8(buffer, 0xf2);
				emit_rex(buffer, false, destination_xmm, 0, source_xmm);
				put_u8(buffer, 0x0f);
				put_u8(buffer, opcode);
				emit_modrm(buffer, 3, destination_xmm, source_xmm);
				return;
			}
		}
	}
	if(!strcmp(line, "not rax")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0xf7);
		put_u8(buffer, 0xd0);
		return;
	}
	if(!strcmp(line, "neg rax")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0xf7);
		put_u8(buffer, 0xd8);
		return;
	}
	if(!strcmp(line, "idiv r10")) {
		put_u8(buffer, 0x49);
		put_u8(buffer, 0xf7);
		put_u8(buffer, 0xfa);
		return;
	}
	if(!strcmp(line, "shl rax, cl")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0xd3);
		put_u8(buffer, 0xe0);
		return;
	}
	if(!strcmp(line, "sar rax, cl")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0xd3);
		put_u8(buffer, 0xf8);
		return;
	}
	if(!strncmp(line, "set", 3) && strstr(line, " al")) {
		uint8_t condition;
		char condition_code[8];
		if(sscanf(line, "set%7s al", condition_code) != 1)
			fatal("internal assembler: %s", line);
		if(!strcmp(condition_code, "e"))
			condition = 0x94;
		else if(!strcmp(condition_code, "ne"))
			condition = 0x95;
		else if(!strcmp(condition_code, "l"))
			condition = 0x9c;
		else if(!strcmp(condition_code, "le"))
			condition = 0x9e;
		else if(!strcmp(condition_code, "g"))
			condition = 0x9f;
		else if(!strcmp(condition_code, "ge"))
			condition = 0x9d;
		else
			fatal("unsupported condition code %s", condition_code);
		put_u8(buffer, 0x0f);
		put_u8(buffer, condition);
		put_u8(buffer, 0xc0);
		return;
	}
	if(sscanf(line, "push %31s", reg1) == 1 &&
	   register_number(reg1) >= 0)
	{
		first_register = register_number(reg1);
		if(first_register >= 8)
			put_u8(buffer, 0x41);
		put_u8(buffer, (uint8_t)(0x50 + (first_register & 7)));
		return;
	}
	if(sscanf(line, "pop %31s", reg1) == 1 &&
	   register_number(reg1) >= 0)
	{
		first_register = register_number(reg1);
		if(first_register >= 8)
			put_u8(buffer, 0x41);
		put_u8(buffer, (uint8_t)(0x58 + (first_register & 7)));
		return;
	}
	if(sscanf(line, "jmp %255s", name) == 1) {
		emit_relative_fixup(array, 0xe9, name, false, 0);
		return;
	}
	if(sscanf(line, "jz %255s", name) == 1) {
		emit_relative_fixup(array, 0, name, true, 0x84);
		return;
	}
	if(sscanf(line, "jnz %255s", name) == 1) {
		emit_relative_fixup(array, 0, name, true, 0x85);
		return;
	}
	if(sscanf(line, "jb %255s", name) == 1) {
		emit_relative_fixup(array, 0, name, true, 0x82);
		return;
	}
	if(sscanf(line, "ja %255s", name) == 1) {
		emit_relative_fixup(array, 0, name, true, 0x87);
		return;
	}
	if(sscanf(line, "jl %255s", name) == 1) {
		emit_relative_fixup(array, 0, name, true, 0x8c);
		return;
	}
	if(sscanf(line, "jg %255s", name) == 1) {
		emit_relative_fixup(array, 0, name, true, 0x8f);
		return;
	}
	if(sscanf(line, "call %255s", name) == 1) {
		char *suffix = strstr(name, "@PLT");
		if(suffix)
			*suffix = 0;
		put_u8(buffer, 0xe8);
		add_fixup(array, FIX_REL32_SYMBOL, buffer->n, name, 0);
		put_u32(buffer, 0);
		add_import(array, name, false);
		return;
	}
	if(sscanf(line, "mov %31[^,], %31s", reg1, reg2) == 2) {
		first_register = register_number(reg1);
		second_register = register_number(reg2);
		if(first_register >= 0 && second_register >= 0) {
			emit_reg_reg(buffer, 0x89, first_register, second_register);
			return;
		}
	}
	if(sscanf(line, "mov rax, %llu", &unsigned_value) == 1) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0xb8);
		put_u64(buffer, (uint64_t)unsigned_value);
		return;
	}
	if(sscanf(line, "mov eax, %lld", &signed_value) == 1) {
		put_u8(buffer, 0xb8);
		put_u32(buffer, (uint32_t)signed_value);
		return;
	}
	if(sscanf(line, "lea %31[^,], [rbp-%ld]", reg1, &displacement) == 2) {
		first_register = register_number(reg1);
		if(first_register < 0)
			fatal("bad register in %s", line);
		emit_lea_memory(buffer, first_register, 5, -displacement);
		return;
	}
	if(sscanf(line, "lea %31[^,], [rbp+%ld]", reg1, &displacement) == 2) {
		first_register = register_number(reg1);
		if(first_register < 0)
			fatal("bad register in %s", line);
		emit_lea_memory(buffer, first_register, 5, displacement);
		return;
	}
	if(sscanf(line, "lea rax, [rip+%255[^]]]", name) == 1) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x8d);
		put_u8(buffer, 0x05);
		add_fixup(array, FIX_RIP32_SYMBOL, buffer->n, name, 0);
		put_u32(buffer, 0);
		return;
	}
	if(sscanf(line, "movzx eax, byte ptr [rip+%255[^]]]", name) == 1) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x8b);
		put_u8(buffer, 0x05);
		add_fixup(array, FIX_RIP32_OBJECT, buffer->n, name, 0);
		put_u32(buffer, 0);
		put_u8(buffer, 0x0f);
		put_u8(buffer, 0xb6);
		put_u8(buffer, 0x00);
		add_import(array, name, true);
		return;
	}
	if(sscanf(line, "movzx eax, word ptr [rip+%255[^]]]", name) == 1) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x8b);
		put_u8(buffer, 0x05);
		add_fixup(array, FIX_RIP32_OBJECT, buffer->n, name, 0);
		put_u32(buffer, 0);
		put_u8(buffer, 0x0f);
		put_u8(buffer, 0xb7);
		put_u8(buffer, 0x00);
		add_import(array, name, true);
		return;
	}
	if(sscanf(line, "movsxd rax, dword ptr [rip+%255[^]]]", name) == 1) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x8b);
		put_u8(buffer, 0x05);
		add_fixup(array, FIX_RIP32_OBJECT, buffer->n, name, 0);
		put_u32(buffer, 0);
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x63);
		put_u8(buffer, 0x00);
		add_import(array, name, true);
		return;
	}
	if(sscanf(line, "mov eax, dword ptr [rip+%255[^]]]", name) == 1) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x8b);
		put_u8(buffer, 0x05);
		add_fixup(array, FIX_RIP32_OBJECT, buffer->n, name, 0);
		put_u32(buffer, 0);
		put_u8(buffer, 0x8b);
		put_u8(buffer, 0x00);
		add_import(array, name, true);
		return;
	}
	if(sscanf(line, "mov rax, qword ptr [rip+%255[^]]]", name) == 1) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x8b);
		put_u8(buffer, 0x05);
		add_fixup(array, FIX_RIP32_OBJECT, buffer->n, name, 0);
		put_u32(buffer, 0);
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x8b);
		put_u8(buffer, 0x00);
		add_import(array, name, true);
		return;
	}
	if(sscanf(line, "mov rax, qword ptr [rbp+%ld]", &displacement) == 1) {
		emit_mov_register_memory(buffer, 0, 5, displacement, 8);
		return;
	}
	if(sscanf(line, "mov qword ptr [rbp-%ld], %31s", &displacement, reg1) == 2) {
		first_register = register_number(reg1);
		if(first_register < 0)
			fatal("bad register in %s", line);
		emit_mov_memory_register(buffer, 5, -displacement, first_register, 8);
		return;
	}
	if(sscanf(line, "mov qword ptr [rax+%ld], %31s", &displacement, reg1) == 2) {
		first_register = register_number(reg1);
		if(first_register < 0)
			fatal("bad register in %s", line);
		emit_mov_memory_register(buffer, 0, displacement, first_register, 8);
		return;
	}
	if(!strcmp(line, "mov byte ptr [rax], 0")) {
		put_u8(buffer, 0xc6);
		put_u8(buffer, 0x00);
		put_u8(buffer, 0x00);
		return;
	}
	if(sscanf(line, "mov qword ptr [rax], %31s", reg1) == 1) {
		first_register = register_number(reg1);
		if(first_register < 0)
			fatal("bad register in %s", line);
		emit_mov_memory_register(buffer, 0, 0, first_register, 8);
		return;
	}
	if(sscanf(line, "mov dword ptr [rax+%ld], %lld", &displacement, &signed_value) == 2) {
		put_u8(buffer, 0xc7);
		emit_memory_operand(buffer, 0, 0, displacement);
		put_u32(buffer, (uint32_t)signed_value);
		return;
	}
	if(sscanf(line, "mov dword ptr [rax], %lld", &signed_value) == 1) {
		put_u8(buffer, 0xc7);
		emit_memory_operand(buffer, 0, 0, 0);
		put_u32(buffer, (uint32_t)signed_value);
		return;
	}
	if(sscanf(line, "sub rsp, %lld", &signed_value) == 1 ||
	   sscanf(line, "add rsp, %lld", &signed_value) == 1)
	{
		bool add = !strncmp(line, "add", 3);
		put_u8(buffer, 0x48);
		if(signed_value >= -128 && signed_value <= 127) {
			put_u8(buffer, 0x83);
			put_u8(buffer, add ? 0xc4 : 0xec);
			put_u8(buffer, (uint8_t)signed_value);
		} else {
			put_u8(buffer, 0x81);
			put_u8(buffer, add ? 0xc4 : 0xec);
			put_u32(buffer, (uint32_t)signed_value);
		}
		return;
	}
	if(sscanf(line, "imul rax, %lld", &signed_value) == 1) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x69);
		put_u8(buffer, 0xc0);
		put_u32(buffer, (uint32_t)signed_value);
		return;
	}
	if(sscanf(line, "add rax, %lld", &signed_value) == 1 ||
	   sscanf(line, "sub rax, %lld", &signed_value) == 1)
	{
		bool add = !strncmp(line, "add", 3);
		put_u8(buffer, 0x48);
		if(signed_value >= -128 && signed_value <= 127) {
			put_u8(buffer, 0x83);
			put_u8(buffer, add ? 0xc0 : 0xe8);
			put_u8(buffer, (uint8_t)signed_value);
		} else {
			put_u8(buffer, add ? 0x05 : 0x2d);
			put_u32(buffer, (uint32_t)signed_value);
		}
		return;
	}
	if(!strcmp(line, "add rax, rcx")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x01);
		put_u8(buffer, 0xc8);
		return;
	}
	if(!strcmp(line, "sub rcx, rax")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x29);
		put_u8(buffer, 0xc1);
		return;
	}
	if(!strcmp(line, "imul rax, rcx")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x0f);
		put_u8(buffer, 0xaf);
		put_u8(buffer, 0xc1);
		return;
	}
	if(!strcmp(line, "and rax, rcx")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x21);
		put_u8(buffer, 0xc8);
		return;
	}
	if(!strcmp(line, "or rax, rcx")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x09);
		put_u8(buffer, 0xc8);
		return;
	}
	if(!strcmp(line, "xor rax, rcx")) {
		put_u8(buffer, 0x48);
		put_u8(buffer, 0x31);
		put_u8(buffer, 0xc8);
		return;
	}
	if(sscanf(line, "%127[^,], %127s", left, right) == 2)
		fatal("unsupported internal assembly instruction: %s, %s", left, right);
	fatal("unsupported internal assembly instruction: %s", line);
}

static void align_assembly_section(AsmImage *array, uint64_t alignment)
{
	if(array->section == ASEC_BSS) {
		array->bss_size = ualign(array->bss_size, alignment);
		return;
	}
	while(current_buffer(array)->n % alignment)
		put_u8(current_buffer(array), 0);
}

static void parse_byte_directive(Buf *buffer, const char *line)
{
	const char *position = line + 5;
	while(*position) {
		char *end;
		unsigned long value;
		while(*position == ' ' || *position == '\t' || *position == ',')
			position++;
		if(!*position)
			break;
		value = strtoul(position, &end, 0);
		if(end == position || value > 255)
			fatal("bad .byte directive");
		put_u8(buffer, (uint8_t)value);
		position = end;
	}
}

static void add_startup(AsmImage *array)
{
	Buf *buffer = &array->text;
	array->section = ASEC_TEXT;
	add_label(array, "_start");
	put_u8(buffer, 0x31);
	put_u8(buffer, 0xed);
	put_u8(buffer, 0x48);
	put_u8(buffer, 0x8b);
	put_u8(buffer, 0x3c);
	put_u8(buffer, 0x24);
	put_u8(buffer, 0x48);
	put_u8(buffer, 0x8d);
	put_u8(buffer, 0x74);
	put_u8(buffer, 0x24);
	put_u8(buffer, 0x08);
	put_u8(buffer, 0x48);
	put_u8(buffer, 0x83);
	put_u8(buffer, 0xe4);
	put_u8(buffer, 0xf0);
	put_u8(buffer, 0xe8);
	add_fixup(array, FIX_REL32_SYMBOL, buffer->n, "_modulon_init", 0);
	put_u32(buffer, 0);
	put_u8(buffer, 0xe8);
	add_fixup(array, FIX_REL32_SYMBOL, buffer->n, "main", 0);
	put_u32(buffer, 0);
	put_u8(buffer, 0x89);
	put_u8(buffer, 0xc7);
	put_u8(buffer, 0xb8);
	put_u32(buffer, 60);
	put_u8(buffer, 0x0f);
	put_u8(buffer, 0x05);
}

void internal_assemble(const char *assembly, AsmImage *array)
{
	char *copy = xstrdup(assembly);
	char *save = NULL;
	char *raw;
	memset(array, 0, sizeof(*array));
	add_startup(array);
	for(raw = strtok_r(copy, "\n", &save); raw;
	    raw = strtok_r(NULL, "\n", &save))
	{
		char *line = trim(raw);
		size_t length;
		if(!*line)
			continue;
		if(!strncmp(line, ".intel_syntax", 13) ||
		   !strncmp(line, ".globl", 6) ||
		   !strncmp(line, ".type", 5) ||
		   !strncmp(line, ".size", 5) ||
		   !strncmp(line, ".section .note", 14))
			continue;
		if(!strcmp(line, ".text")) {
			array->section = ASEC_TEXT;
			continue;
		}
		if(!strcmp(line, ".bss")) {
			array->section = ASEC_BSS;
			continue;
		}
		if(!strcmp(line, ".section .rodata")) {
			array->section = ASEC_RODATA;
			continue;
		}
		if(!strncmp(line, ".align ", 7)) {
			align_assembly_section(array, strtoull(line + 7, NULL, 0));
			continue;
		}
		if(!strncmp(line, ".zero ", 6)) {
			uint64_t count = strtoull(line + 6, NULL, 0);
			if(array->section == ASEC_BSS)
				array->bss_size += count;
			else
				while(count--)
					put_u8(current_buffer(array), 0);
			continue;
		}
		if(!strncmp(line, ".byte", 5)) {
			parse_byte_directive(current_buffer(array), line);
			continue;
		}
		length = strlen(line);
		if(length && line[length - 1] == ':') {
			line[length - 1] = 0;
			add_label(array, line);
			continue;
		}
		if(array->section != ASEC_TEXT)
			fatal("instruction outside text section: %s", line);
		assemble_instruction(array, line);
	}
	free(copy);
}

static void append_plt(AsmImage *array)
{
	Buf *buffer = &array->text;
	size_t index;
	uint32_t relocation_index = 0;
	while(buffer->n & 15)
		put_u8(buffer, 0x90);
	array->plt0_offset = buffer->n;
	put_u8(buffer, 0xff);
	put_u8(buffer, 0x35);
	add_fixup(array, FIX_RIP32_GOT, buffer->n, NULL, 1);
	put_u32(buffer, 0);
	put_u8(buffer, 0xff);
	put_u8(buffer, 0x25);
	add_fixup(array, FIX_RIP32_GOT, buffer->n, NULL, 2);
	put_u32(buffer, 0);
	put_u8(buffer, 0x0f);
	put_u8(buffer, 0x1f);
	put_u8(buffer, 0x40);
	put_u8(buffer, 0x00);
	for(index = 0; index < array->nimports; index++) {
		Import *import = &array->imports[index];
		if(import->object)
			continue;
		import->plt_offset = buffer->n;
		import->plt_reloc_index = relocation_index++;
		put_u8(buffer, 0xff);
		put_u8(buffer, 0x25);
		add_fixup(array, FIX_RIP32_GOT, buffer->n, NULL, import->got_index);
		put_u32(buffer, 0);
		put_u8(buffer, 0x68);
		put_u32(buffer, import->plt_reloc_index);
		put_u8(buffer, 0xe9);
		add_fixup(array, FIX_REL32_SYMBOL, buffer->n, "@plt0", 0);
		put_u32(buffer, 0);
	}
}

static void prune_local_imports(AsmImage *array)
{
	size_t read_index;
	size_t write_index = 0;
	for(read_index = 0; read_index < array->nimports; read_index++) {
		Import *import = &array->imports[read_index];
		if(find_label(array, import->name))
			continue;
		if(write_index != read_index)
			array->imports[write_index] = *import;
		write_index++;
	}
	array->nimports = write_index;
}

static uint32_t sysv_hash(const unsigned char *name)
{
	uint32_t hash = 0;
	while(*name) {
		uint32_t high;
		hash = (hash << 4) + *name++;
		high = hash & 0xf0000000;
		if(high)
			hash ^= high >> 24;
		hash &= ~high;
	}
	return hash;
}

static uint64_t symbol_address(AsmImage *array_1, ALabel *label, uint64_t text_va, uint64_t rodata_va, uint64_t bss_va)
{
	(void)array_1;
	if(label->section == ASEC_TEXT)
		return text_va + label->offset;
	if(label->section == ASEC_RODATA)
		return rodata_va + label->offset;
	return bss_va + label->offset;
}

static Import *require_import(AsmImage *array, const char *name, bool object)
{
	Import *import = add_import(array, name, object);
	if(object)
		import->object = true;
	return import;
}

static void patch_code(AsmImage *array, uint64_t text_va, uint64_t rodata_va, uint64_t bss_va, uint64_t got_va)
{
	size_t index;
	for(index = 0; index < array->nfixups; index++) {
		Fixup *fix = &array->fixups[index];
		uint64_t place = text_va + fix->offset;
		uint64_t target = 0;
		int64_t relative;
		if(fix->kind == FIX_RIP32_GOT) {
			target = got_va + (uint64_t)fix->aux * 8;
		} else if(fix->name && !strcmp(fix->name, "@plt0"))
		{
			target = text_va + array->plt0_offset;
		} else {
			ALabel *label = fix->name ? find_label(array, fix->name) : NULL;
			if(label)
				target = symbol_address(array, label, text_va, rodata_va, bss_va);
			else {
				Import *import;
				if(fix->kind == FIX_RIP32_OBJECT)
					import = require_import(array, fix->name, true);
				else
					import = require_import(array, fix->name, false);
				if(fix->kind == FIX_RIP32_OBJECT)
					target = got_va + (uint64_t)import->got_index * 8;
				else
					target = text_va + import->plt_offset;
			}
		}
		relative = (int64_t)target - (int64_t)(place + 4);
		if(relative < INT32_MIN || relative > INT32_MAX)
			fatal("internal: relative relocation overflow");
		patch_u32(&array->text, fix->offset, (uint32_t)(int32_t)relative);
	}
}

static void copy_into(uint8_t *file, uint64_t offset, const void *data, size_t size)
{
    /* Empty sections may have no backing allocation. */
    if(size)
		memcpy(file + offset, data, size);
}

void write_independent_elf(const char *path, AsmImage *array, char **libraries, size_t library_count)
{
	const uint64_t base = 0x400000;
	const char interpreter[] = "/lib64/ld-linux-x86-64.so.2";
	const uint16_t phnum = 5;
	uint64_t cursor;
	uint64_t interp_off, text_off, rodata_off, dynstr_off, dynsym_off;
	uint64_t hash_off, rela_dyn_off, rela_plt_off, got_off, dynamic_off;
	uint64_t bss_off, file_size, memory_size;
	uint64_t text_va, rodata_va, bss_va, got_va, dynamic_va;
	Buf dynstr = {0};
	Elf64_Sym *dynsym;
	Elf64_Rela *rela_dyn;
	Elf64_Rela *rela_plt;
	Elf64_Dyn *dynamic;
	uint64_t *got;
	uint32_t *hash;
	uint32_t *buckets;
	uint32_t *chains;
	uint32_t symbol_count;
	uint32_t function_count = 0;
	uint32_t object_count = 0;
	uint32_t bucket_count;
	uint32_t dynamic_count;
	uint32_t needed_count = 1 + (uint32_t)library_count;
	uint32_t *needed_offsets;
	uint8_t *file;
	Elf64_Ehdr *ehdr;
	Elf64_Phdr *phdr;
	size_t index_1;
	uint32_t next_got = 3;
	uint32_t next_symbol = 1;
	uint32_t next_function_relocation = 0;
	uint32_t next_object_relocation = 0;
	prune_local_imports(array);
	put_u8(&dynstr, 0);
	needed_offsets = xcalloc(needed_count, sizeof(*needed_offsets));
	needed_offsets[0] = dynstr.n;
	bputs(&dynstr, "libc.so.6");
	put_u8(&dynstr, 0);
	for(index_1 = 0; index_1 < library_count; index_1++) {
		char library_name[256];
		uint32_t index = 1 + (uint32_t)index_1;
		const char *argument = libraries[index_1];
		if(!strncmp(argument, "-l", 2))
			argument += 2;
		if(strstr(argument, ".so"))
			snprintf(library_name, sizeof(library_name), "%s", argument);
		else
			snprintf(library_name, sizeof(library_name), "lib%s.so", argument);
		needed_offsets[index] = dynstr.n;
		bputs(&dynstr, library_name);
		put_u8(&dynstr, 0);
	}
	for(index_1 = 0; index_1 < array->nimports; index_1++) {
		Import *import = &array->imports[index_1];
		import->symbol_index = next_symbol++;
		import->got_index = next_got++;
		import->string_offset = dynstr.n;
		bputs(&dynstr, import->name);
		put_u8(&dynstr, 0);
		if(import->object)
			object_count++;
		else
			function_count++;
	}
	append_plt(array);
	symbol_count = (uint32_t)array->nimports + 1;
	dynsym = xcalloc(symbol_count, sizeof(*dynsym));
	for(index_1 = 0; index_1 < array->nimports; index_1++) {
		Import *import = &array->imports[index_1];
		Elf64_Sym *symbol = &dynsym[import->symbol_index];
		symbol->st_name = import->string_offset;
		symbol->st_info = ELF64_ST_INFO(STB_GLOBAL,
						import->object ? STT_OBJECT : STT_FUNC);
		symbol->st_other = STV_DEFAULT;
		symbol->st_shndx = SHN_UNDEF;
	}
	bucket_count = symbol_count ? symbol_count : 1;
	hash = xcalloc(2 + bucket_count + symbol_count, sizeof(*hash));
	hash[0] = bucket_count;
	hash[1] = symbol_count;
	buckets = hash + 2;
	chains = buckets + bucket_count;
	for(index_1 = 1; index_1 < symbol_count; index_1++) {
		uint32_t bucket = sysv_hash((unsigned char *)array->imports[index_1 - 1].name) % bucket_count;
		if(!buckets[bucket])
			buckets[bucket] = (uint32_t)index_1;
		else {
			uint32_t link = buckets[bucket];
			while(chains[link])
				link = chains[link];
			chains[link] = (uint32_t)index_1;
		}
	}
	rela_dyn = xcalloc(object_count ? object_count : 1, sizeof(*rela_dyn));
	rela_plt = xcalloc(function_count ? function_count : 1, sizeof(*rela_plt));
	got = xcalloc(3 + array->nimports, sizeof(*got));
	cursor = ualign(sizeof(Elf64_Ehdr) + phnum * sizeof(Elf64_Phdr), 16);
	interp_off = cursor;
	cursor += sizeof(interpreter);
	text_off = ualign(cursor, 16);
	cursor = text_off + array->text.n;
	rodata_off = ualign(cursor, 16);
	cursor = rodata_off + array->rodata.n;
	dynstr_off = ualign(cursor, 8);
	cursor = dynstr_off + dynstr.n;
	dynsym_off = ualign(cursor, 8);
	cursor = dynsym_off + symbol_count * sizeof(Elf64_Sym);
	hash_off = ualign(cursor, 8);
	cursor = hash_off + (2 + bucket_count + symbol_count) * sizeof(uint32_t);
	rela_dyn_off = ualign(cursor, 8);
	cursor = rela_dyn_off + object_count * sizeof(Elf64_Rela);
	rela_plt_off = ualign(cursor, 8);
	cursor = rela_plt_off + function_count * sizeof(Elf64_Rela);
	got_off = ualign(cursor, 8);
	cursor = got_off + (3 + array->nimports) * sizeof(uint64_t);
	dynamic_count = needed_count + 12 + 1;
	dynamic_off = ualign(cursor, 8);
	cursor = dynamic_off + dynamic_count * sizeof(Elf64_Dyn);
	file_size = ualign(cursor, 16);
	bss_off = file_size;
	memory_size = bss_off + array->bss_size;
	text_va = base + text_off;
	rodata_va = base + rodata_off;
	bss_va = base + bss_off;
	got_va = base + got_off;
	dynamic_va = base + dynamic_off;
	got[0] = dynamic_va;
	for(index_1 = 0; index_1 < array->nimports; index_1++) {
		Import *import = &array->imports[index_1];
		if(import->object) {
			Elf64_Rela *relocation = &rela_dyn[next_object_relocation++];
			relocation->r_offset = got_va + (uint64_t)import->got_index * 8;
			relocation->r_info = ELF64_R_INFO(import->symbol_index,
							  R_X86_64_GLOB_DAT);
			relocation->r_addend = 0;
		} else {
			Elf64_Rela *relocation = &rela_plt[next_function_relocation++];
			relocation->r_offset = got_va + (uint64_t)import->got_index * 8;
			relocation->r_info = ELF64_R_INFO(import->symbol_index,
							  R_X86_64_JUMP_SLOT);
			relocation->r_addend = 0;
			got[import->got_index] = text_va + import->plt_offset + 6;
		}
	}
	patch_code(array, text_va, rodata_va, bss_va, got_va);
	dynamic = xcalloc(dynamic_count, sizeof(*dynamic));
	{
		uint32_t value = 0;
		for(index_1 = 0; index_1 < needed_count; index_1++) {
			dynamic[value].d_tag = DT_NEEDED;
			dynamic[value++].d_un.d_val = needed_offsets[index_1];
		}
		dynamic[value].d_tag = DT_HASH;
		dynamic[value++].d_un.d_ptr = base + hash_off;
		dynamic[value].d_tag = DT_STRTAB;
		dynamic[value++].d_un.d_ptr = base + dynstr_off;
		dynamic[value].d_tag = DT_SYMTAB;
		dynamic[value++].d_un.d_ptr = base + dynsym_off;
		dynamic[value].d_tag = DT_STRSZ;
		dynamic[value++].d_un.d_val = dynstr.n;
		dynamic[value].d_tag = DT_SYMENT;
		dynamic[value++].d_un.d_val = sizeof(Elf64_Sym);
		dynamic[value].d_tag = DT_PLTGOT;
		dynamic[value++].d_un.d_ptr = got_va;
		dynamic[value].d_tag = DT_PLTRELSZ;
		dynamic[value++].d_un.d_val = function_count * sizeof(Elf64_Rela);
		dynamic[value].d_tag = DT_PLTREL;
		dynamic[value++].d_un.d_val = DT_RELA;
		dynamic[value].d_tag = DT_JMPREL;
		dynamic[value++].d_un.d_ptr = base + rela_plt_off;
		dynamic[value].d_tag = DT_RELA;
		dynamic[value++].d_un.d_ptr = base + rela_dyn_off;
		dynamic[value].d_tag = DT_RELASZ;
		dynamic[value++].d_un.d_val = object_count * sizeof(Elf64_Rela);
		dynamic[value].d_tag = DT_RELAENT;
		dynamic[value++].d_un.d_val = sizeof(Elf64_Rela);
		dynamic[value].d_tag = DT_NULL;
	}
	file = xcalloc(file_size, 1);
	ehdr = (Elf64_Ehdr *)file;
	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = ELFCLASS64;
	ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_ident[EI_OSABI] = ELFOSABI_SYSV;
	ehdr->e_type = ET_EXEC;
	ehdr->e_machine = EM_X86_64;
	ehdr->e_version = EV_CURRENT;
	ehdr->e_entry = text_va;
	ehdr->e_phoff = sizeof(Elf64_Ehdr);
	ehdr->e_ehsize = sizeof(Elf64_Ehdr);
	ehdr->e_phentsize = sizeof(Elf64_Phdr);
	ehdr->e_phnum = phnum;
	phdr = (Elf64_Phdr *)(file + ehdr->e_phoff);
	phdr[0].p_type = PT_PHDR;
	phdr[0].p_flags = PF_R;
	phdr[0].p_offset = ehdr->e_phoff;
	phdr[0].p_vaddr = base + ehdr->e_phoff;
	phdr[0].p_paddr = phdr[0].p_vaddr;
	phdr[0].p_filesz = phnum * sizeof(Elf64_Phdr);
	phdr[0].p_memsz = phdr[0].p_filesz;
	phdr[0].p_align = 8;
	phdr[1].p_type = PT_INTERP;
	phdr[1].p_flags = PF_R;
	phdr[1].p_offset = interp_off;
	phdr[1].p_vaddr = base + interp_off;
	phdr[1].p_paddr = phdr[1].p_vaddr;
	phdr[1].p_filesz = sizeof(interpreter);
	phdr[1].p_memsz = sizeof(interpreter);
	phdr[1].p_align = 1;
	phdr[2].p_type = PT_LOAD;
	phdr[2].p_flags = PF_R | PF_W | PF_X;
	phdr[2].p_offset = 0;
	phdr[2].p_vaddr = base;
	phdr[2].p_paddr = base;
	phdr[2].p_filesz = file_size;
	phdr[2].p_memsz = memory_size;
	phdr[2].p_align = 0x1000;
	phdr[3].p_type = PT_DYNAMIC;
	phdr[3].p_flags = PF_R | PF_W;
	phdr[3].p_offset = dynamic_off;
	phdr[3].p_vaddr = dynamic_va;
	phdr[3].p_paddr = dynamic_va;
	phdr[3].p_filesz = dynamic_count * sizeof(Elf64_Dyn);
	phdr[3].p_memsz = phdr[3].p_filesz;
	phdr[3].p_align = 8;
	phdr[4].p_type = PT_GNU_STACK;
	phdr[4].p_flags = PF_R | PF_W;
	phdr[4].p_align = 16;
	copy_into(file, interp_off, interpreter, sizeof(interpreter));
	copy_into(file, text_off, array->text.s, array->text.n);
	copy_into(file, rodata_off, array->rodata.s, array->rodata.n);
	copy_into(file, dynstr_off, dynstr.s, dynstr.n);
	copy_into(file, dynsym_off, dynsym, symbol_count * sizeof(Elf64_Sym));
	copy_into(file, hash_off, hash, (2 + bucket_count + symbol_count) * sizeof(uint32_t));
	copy_into(file, rela_dyn_off, rela_dyn, object_count * sizeof(Elf64_Rela));
	copy_into(file, rela_plt_off, rela_plt, function_count * sizeof(Elf64_Rela));
	copy_into(file, got_off, got, (3 + array->nimports) * sizeof(uint64_t));
	copy_into(file, dynamic_off, dynamic, dynamic_count * sizeof(Elf64_Dyn));
	write_file(path, (char *)file, file_size);
	if(chmod(path, 0755))
		fatal("cannot make %s executable: %s", path, strerror(errno));
}
