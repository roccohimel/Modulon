#ifndef JIT_H
#define JIT_H

#include "parser.h"

/*
Modulon JIT.
Compiles a parsed program straight to executable memory and lets the host
process call into it, with no object files, no ELF, and no exec().
Pipeline: generate() -> internal_assemble() -> map + link in memory.
*/

typedef struct JitModule JitModule;

/*
Custom symbol resolution for imported (external) names.
Return the address of the symbol, or NULL if unknown. Pass NULL to use
the default resolver, which looks names up with dlsym() in the host
executable and its loaded libraries (libc included).
*/
typedef void *(*JitResolver)(const char *name, void *context);

/*Compile the program and return a live module, or die via fatal().*/
JitModule *jit_compile(Program *program, JitResolver resolver, void *resolver_context);

/*Address of an internal symbol (function, global, or string label), or NULL.*/
void *jit_symbol(JitModule *module, const char *name);

/*
Call an entry function as int entry(int argc, char **argv).
entry may be NULL for "main". Returns the function's return value.
*/
int jit_run(JitModule *module, const char *entry, int argc, char **argv);

/*Release a module and all memory it owns. Executed code must not run after.*/
void jit_free(JitModule *module);

#endif
