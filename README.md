# The Modulon Programming Language
Modulon is a unified compiler and programming language designed for fast
compilation, native execution, JIT/AOT compilation, and low-level programming
without the complexity of a traditional compiler toolchain.

# Downloading Modulon
Latest releases: https://github.com/roccohimel/Modulon/releases

# Building Modulon from Source
**Required build dependencies**

- GCC
- ANSI C libraries

**Building Modulon**

To build Modulon from source, clone the repository. You must have git for this
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
Once Modulon has finished compiling, the compiler should be located:
```
bin/mlonc
```

# Running Programs (JIT)
You can compile and run a Modulon program directly in memory, with no
output binary and no exec():
```
./bin/mlonc --jit program.c
```
The JIT uses the same code generation and assembly as the native path,
then links the machine code straight into the Modulon process: a GOT and
small jump thunks are built for external symbols (resolved through the
host dynamic linker), all relocations are patched in memory, and the
code pages are locked to read+execute before `main` is called. The
program's exit code becomes the compiler's exit code.

The JIT is also usable as a library (include/jit.h): `jit_compile()`,
`jit_symbol()`, `jit_run()`, and `jit_free()`, with an optional custom
symbol resolver for embedding Modulon in another program.

# Credits
The Modulon Language is maintained and lead by me, Rocco Himel.
