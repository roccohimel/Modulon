#define _GNU_SOURCE
#include "jit.h"

#include "codegen.h"
#include "assembler.h"

#include <sys/mman.h>
#include <dlfcn.h>

/*
In-memory linker.
The assembler already hands us everything we need in an AsmImage: machine
code, rodata, bss size, a label table, and a fixup list describing every
32-bit patch site (relative branches/calls and RIP-relative accesses).
Where write_independent_elf() would emit an ELF with a PLT and GOT and let
the dynamic linker resolve imports, we do the same job ourselves.
Every import gets a GOT slot and a 6-byte thunk (jmp *got_slot) so that
rel32 calls and RIP-relative loads stay in range. The mapping is built
writable, patched, then locked to read+execute, so no page is ever both
writable and executable at the same time.
*/

struct JitModule
{
	uint8_t *code;
	size_t code_size;
	size_t exec_size;
	uint64_t text_va;
	uint64_t thunk_va;
	uint64_t rodata_va;
	uint64_t got_va;
	uint64_t data_va;
	ALabel *labels;
	size_t nlabels;
};

typedef struct
{
	char *name;
	uint64_t got_va;
	uint64_t thunk_va;
	bool object;
} JitImport;

typedef struct
{
	JitImport *imports;
	size_t nimports, capimports;
	uint64_t *got;
	size_t ngot;
} JitLink;

static uint64_t jalign(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static void *default_resolver(const char *name, void *context)
{
	static void *self;
	(void)context;
	if(!self)
		self = dlopen(NULL, RTLD_LAZY);
	return dlsym(self, name);
}

static uint64_t label_address(JitModule *module, ALabel *label)
{
	if(label->section == ASEC_TEXT)
		return module->text_va + label->offset;
	if(label->section == ASEC_RODATA)
		return module->rodata_va + label->offset;
	return module->data_va + label->offset;
}

static ALabel *module_label(JitModule *module, const char *name)
{
	size_t index;
	for(index = 0; index < module->nlabels; index++)
		if(!strcmp(module->labels[index].name, name))
			return &module->labels[index];
	return NULL;
}

static JitImport *link_import(JitLink *link, AsmImage *image, Import *import)
{
	size_t index;
	for(index = 0; index < link->nimports; index++)
		if(!strcmp(link->imports[index].name, import->name))
			return &link->imports[index];
	ARR_GROW(link->imports, link->nimports, link->capimports, JitImport);
	link->imports[link->nimports].name = import->name;
	link->imports[link->nimports].object = import->object;
	link->nimports++;
	return &link->imports[link->nimports - 1];
}

static void patch_u32_at(uint8_t *code, uint64_t offset, uint32_t value)
{
	code[offset + 0] = (uint8_t)value;
	code[offset + 1] = (uint8_t)(value >> 8);
	code[offset + 2] = (uint8_t)(value >> 16);
	code[offset + 3] = (uint8_t)(value >> 24);
}

static void jit_link_code(JitModule *module, AsmImage *image, JitLink *link)
{
	size_t index;
	for(index = 0; index < image->nfixups; index++) {
		Fixup *fix = &image->fixups[index];
		uint64_t place = module->text_va + fix->offset;
		uint64_t target = 0;
		int64_t relative;
		if(fix->kind == FIX_RIP32_GOT ||
		   (fix->name && !strcmp(fix->name, "@plt0")))
			fatal("JIT: internal PLT fixup encountered");
		else {
			ALabel *label = fix->name ? module_label(module, fix->name) : NULL;
			if(label)
				target = label_address(module, label);
			else {
				JitImport *slot = NULL;
				size_t jindex;
				if(!fix->name)
					fatal("JIT: fixup with no symbol name");
				for(jindex = 0; jindex < link->nimports; jindex++)
					if(!strcmp(link->imports[jindex].name, fix->name)) {
						slot = &link->imports[jindex];
						break;
					}
				if(!slot)
					fatal("JIT: unresolved import %s", fix->name);
				if(fix->kind == FIX_RIP32_OBJECT)
					target = slot->got_va;
				else
					target = slot->thunk_va;
			}
		}
		relative = (int64_t)target - (int64_t)(place + 4);
		if(relative < INT32_MIN || relative > INT32_MAX)
			fatal("JIT: relocation overflow for %s", fix->name ? fix->name : "?");
		patch_u32_at(module->code, fix->offset, (uint32_t)(int32_t)relative);
	}
}

JitModule *jit_compile(Program *program, JitResolver resolver, void *resolver_context)
{
	JitModule *module = xcalloc(1, sizeof(JitModule));
	JitLink link = {0};
	AsmImage image;
	char *assembly;
	uint64_t thunk_off, rodata_off, got_off, bss_off, exec_size, code_size;
	size_t index;
	if(!resolver)
		resolver = default_resolver;
	assembly = generate(program);
	internal_assemble(assembly, &image);
	free(assembly);

	//Layout. Imports that also exist as labels are internal symbols, not imports.
	for(index = 0; index < image.nimports; index++) {
		ALabel *label = NULL;
		size_t lindex;
		for(lindex = 0; lindex < image.nlabels; lindex++)
			if(!strcmp(image.labels[lindex].name, image.imports[index].name)) {
				label = &image.labels[lindex];
				break;
			}
		if(!label) {
			JitImport *slot = link_import(&link, &image, &image.imports[index]);
			(void)slot;
		}
	}
	thunk_off = jalign(image.text.n, 16);
	rodata_off = jalign(thunk_off + link.nimports * 16, 16);
	got_off = jalign(rodata_off + image.rodata.n, 8);
	bss_off = jalign(got_off + link.nimports * 8, 4096);
	exec_size = bss_off;
	code_size = bss_off + jalign(image.bss_size, 4096);

	module->code = mmap(NULL, code_size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(module->code == MAP_FAILED)
		fatal("JIT: mmap failed");
	module->code_size = code_size;
	module->text_va = (uint64_t)(uintptr_t)module->code;
	module->thunk_va = module->text_va + thunk_off;
	module->rodata_va = module->text_va + rodata_off;
	module->got_va = module->text_va + got_off;
	module->data_va = module->text_va + bss_off;
	module->exec_size = exec_size;
	module->labels = image.labels;
	module->nlabels = image.nlabels;

	memcpy(module->code, image.text.s, image.text.n);
	if(image.rodata.n)
		memcpy(module->code + rodata_off, image.rodata.s, image.rodata.n);

	//Resolve every import, fill the GOT, and emit a jmp *got(N) thunk per import.
	for(index = 0; index < link.nimports; index++) {
		JitImport *slot = &link.imports[index];
		void *address = resolver(slot->name, resolver_context);
		if(!address)
			fatal("JIT: undefined symbol %s", slot->name);
		slot->got_va = module->got_va + index * 8;
		slot->thunk_va = module->thunk_va + index * 16;
		*(uint64_t *)(module->code + got_off + index * 8) = (uint64_t)(uintptr_t)address;
		{
			uint8_t *thunk = module->code + (slot->thunk_va - module->text_va);
			int64_t disp = (int64_t)slot->got_va - (int64_t)(slot->thunk_va + 6);
			thunk[0] = 0xff;
			thunk[1] = 0x25;
			patch_u32_at(thunk, 2, (uint32_t)(int32_t)disp);
		}
	}

	jit_link_code(module, &image, &link);

	//Lock code, rodata, and the GOT to read+execute; the bss pages stay writable.
	if(mprotect(module->code, exec_size, PROT_READ | PROT_EXEC))
		fatal("JIT: mprotect failed");

	free(image.text.s);
	free(image.rodata.s);
	free(image.fixups);
	free(image.imports);
	free(link.imports);
	return module;
}

void *jit_symbol(JitModule *module, const char *name)
{
	ALabel *label = module_label(module, name);
	if(!label)
		return NULL;
	return (void *)(uintptr_t)label_address(module, label);
}

int jit_run(JitModule *module, const char *entry, int argc, char **argv)
{
	int (*function)(int, char **) = jit_symbol(module, entry ? entry : "main");
	if(!function)
		fatal("JIT: no function named %s", entry ? entry : "main");
	return function(argc, argv);
}

void jit_free(JitModule *module)
{
	if(!module)
		return;
	if(module->code && module->code != MAP_FAILED)
		munmap(module->code, module->code_size);
	free(module->labels);
	free(module);
}
