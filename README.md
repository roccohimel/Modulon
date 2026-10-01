# The Modulon Language Project
Modulon C is a unified compiler and programming language designed for fast
compilation, native execution, JIT/AOT compilation, and low-level programming
without the complexity of a traditional compiler toolchain.

# Downloading Modulon C
Latest releases: https://github.com/roccohimel/Modulon/releases

# Building Modulon C from Source
**Required build dependencies**

- GCC
- ANSI C libraries

**Building Modulon C**

To build Modulon C from source, clone the repository. You must have git for this
to work:
```
git clone https://github.com/roccohimel/Modulon.git
```
Once git has finished cloning, make the build script executable. Then run it.
```
cd Modulon
chmod +x build.sh
./build.sh
```
Once Modulon C has finished compiling, the compiler should be located:
```
bin/mlonc
```

# Running Programs (JIT)
You can compile and run a Modulon C program directly in memory, with no
output binary and no exec():
```
./bin/mlonc --jit program.c
```
The JIT uses the same code generation and assembly as the native path,
then links the machine code straight into the Modulon C process: a GOT and
small jump thunks are built for external symbols (resolved through the
host dynamic linker), all relocations are patched in memory, and the
code pages are locked to read+execute before `main` is called. The
program's exit code becomes the compiler's exit code.

The JIT is also usable as a library (include/jit.h): `jit_compile()`,
`jit_symbol()`, `jit_run()`, and `jit_free()`, with an optional custom
symbol resolver for embedding Modulon C in another program.

# Strings
Modulon C has a built-in `string` type for null-terminated character strings. It can be used for local and global variables, assignments, function parameters, indexing, and `%v` printing:

```mlonc
string greeting = "hello";

int main()
{
    string message = greeting;
    printf("%v\n", message);
    printf("%v\n", message[0]);
    return 0;
}
```

String literals are stored in read-only memory, so their characters must not be modified through a `string` value.




Copyright (c) 2026 The Modulon Software Foundation.
Modulon C is licensed under the GNU General Public License, v3.
For more information, please visit **https://mlonc.modulonsoftware.org/**
## Enums

C-style enums are supported in declarations and expressions, including explicit
integer values, enum variables, and the JIT. See [enum support](ENUMS.md) for
examples, semantics, and regression test commands.
