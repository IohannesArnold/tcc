## Tiny C Compiler

This is a heavily modified version of Fabrice Bellard's
[Tiny C Compiler](https://bellard.org/tcc/). The goal of this fork is to
maintain a compiler that bridges the gap between hobby compilers and OSDEV
projects, and major professional compilers and other software packages. Hence,
compiling this compiler should be as easy and undemanding as possible, but this
compiler should be able to compile major software, including GCC.

### Getting Started

To compile TCC you need:

 - A ninja build system, either: [ninja](https://github.com/ninja-build/ninja)
   or [samurai](https://github.com/michaelforney/samurai)
 - A i386 C compiler

To compile, simply finish the line `cc = ` with your compiler, and `cflags = ` 
with the CFLAGS you want. Then issue the command `ninja`.

### Next Steps

### See Also

These are other internet resources that address similar concerns:

 - https://bootstrappable.org
 - [Bootstrapping Wiki](https://bootstrapping.miraheze.org/wiki/Main_Page)
 - [OSDev Wiki](https://wiki.osdev.org/Main_Page)
 - [musl libc's Suggested Alternative Libraries](https://wiki.musl-libc.org/alternatives.html)

