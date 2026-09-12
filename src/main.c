#include "parser.h"
#include "codegen.h"
#include "assembler.h"
#include "i386.h"
#include "jit.h"

static void usage(void)
{
	printf("%s\nUsage: modulon [options]...\n\n"
	       "  -o FILE          set output file\n"
	       "  -S               emit generated x86-64 assembly text\n"
	       "  -m32 -c          emit an ELF32 i386 relocatable object\n"
	       "  --jit            compile and run in-process (JIT)\n"
	       "  -ffreestanding   accepted for freestanding kernel builds\n"
	       "  -lNAME           add a DT_NEEDED library to x86-64 output\n"
	       "  --version        show version\n\n",
	       VERSION);
}

int main(int argc, char **argv)
{
	Program prog = {0};
	char **src = NULL;
	char **libs = NULL;
	size_t nsrc = 0, csrc = 0, nlibs = 0, clibs = 0, index;
	char *out = xstrdup("a.out");
	bool assembly_only = false;
	bool compile_only = false;
	bool target_i386 = false;
	bool freestanding = false;
	bool run_jit = false;
	size_t first_source = 0;
	for(index = 1; index < (size_t)argc; index++) {
		if(!strcmp(argv[index], "--help") || !strcmp(argv[index], "-h")) {
			usage();
			return 0;
		}
		if(!strcmp(argv[index], "--version")) {
			puts(VERSION);
			return 0;
		}
		if(!strcmp(argv[index], "-o")) {
			if(++index >= (size_t)argc)
				fatal("-o needs a file");
			out = argv[index];
		} else if(!strcmp(argv[index], "-S"))
		{
			assembly_only = true;
		} else if(!strcmp(argv[index], "-c"))
		{
			compile_only = true;
		} else if(!strcmp(argv[index], "--jit"))
		{
			run_jit = true;
		} else if(!strcmp(argv[index], "-m32"))
		{
			target_i386 = true;
		} else if(!strcmp(argv[index], "-ffreestanding"))
		{
			freestanding = true;
		} else if(!strcmp(argv[index], "-fno-builtin") ||
			  !strcmp(argv[index], "-fno-stack-protector") ||
			  !strcmp(argv[index], "-fno-pie") ||
			  !strcmp(argv[index], "-fno-pic") ||
			  !strcmp(argv[index], "-nostdlib") ||
			  !strcmp(argv[index], "-nostdinc") ||
			  !strcmp(argv[index], "-Wall") ||
			  !strcmp(argv[index], "-Wextra") ||
			  !strcmp(argv[index], "-Wpedantic") ||
			  !strncmp(argv[index], "-O", 2) ||
			  !strncmp(argv[index], "-std=", 5) ||
			  !strncmp(argv[index], "-I", 2) ||
			  !strncmp(argv[index], "-D", 2))
		{
			//Accepted compatibility flags; AneoC is always freestanding in -m32 -c mode.
		} else if(!strncmp(argv[index], "-l", 2))
		{
			ARR_GROW(libs, nlibs, clibs, char *);
			libs[nlibs++] = argv[index];
		} else if(argv[index][0] == '-')
		{
			fatal("unsupported option %s", argv[index]);
		} else {
			if(!nsrc)
				first_source = index;
			ARR_GROW(src, nsrc, csrc, char *);
			src[nsrc++] = argv[index];
		}
	}
	if(!nsrc)
		fatal("no input files");
	if(!assembly_only && strlen(out) >= 2 &&
	   !strcmp(out + strlen(out) - 2, ".o"))
	{
		compile_only = true;
		target_i386 = true;
	}
	if(compile_only)
		target_i386 = true;
	type_set_target(target_i386 ? TARGET_I386 : TARGET_POSIX);
	for(index = 0; index < nsrc; index++) {
		char *raw = read_file(src[index]);
		char *text = preprocess_source(raw);
		Tokens tokens = lex_source(text, src[index]);
		parse_program(&tokens, &prog);
	}
	(void)freestanding;
	if(run_jit && (compile_only || assembly_only || target_i386))
		fatal("--jit cannot be combined with -S, -c, or -m32");
	if(run_jit) {
		JitModule *module = jit_compile(&prog, NULL, NULL);
		int code = jit_run(module, "main", (int)(argc - first_source), &argv[first_source]);
		jit_free(module);
		return code;
	}
	if(compile_only) {
		if(assembly_only)
			fatal("-S and -c cannot be combined yet");
		write_i386_relocatable(out, &prog);
		return 0;
	}
	if(target_i386)
		fatal("-m32 currently requires -c");
	{
		char *assembly = generate(&prog);
		if(assembly_only) {
			write_file(out, assembly, strlen(assembly));
			return 0;
		}
		{
			AsmImage image;
			internal_assemble(assembly, &image);
			write_independent_elf(out, &image, libs, nlibs);
		}
	}
	return 0;
}
