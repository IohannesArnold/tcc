/* Simple libc header for TCC 
 * 
 * Add any function you want from the libc there. This file is here
 * only for your convenience so that you do not need to put the whole
 * glibc include files on your floppy disk 
 */

#include <stddef.h>
#include <stdarg.h>

/* stdlib.h */
void *calloc(size_t nmemb, size_t size);
void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
int atoi(const char *nptr);
long int strtol(const char *nptr, char **endptr, int base);
unsigned long int strtoul(const char *nptr, char **endptr, int base);
void exit(int status);

/* stdio.h */
typedef struct __FILE FILE;
#define EOF (-1)
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fildes, const char *mode);
FILE *freopen(const  char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
size_t  fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t  fwrite(void *ptr, size_t size, size_t nmemb, FILE *stream);
int fgetc(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int getc(FILE *stream);
int getc_unlocked(FILE *stream);
int getchar(void);
char *gets(char *s);
int ungetc(int c, FILE *stream);
int fflush(FILE *stream);

int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const  char  *format, ...);
int asprintf(char **strp, const char *format, ...);
int dprintf(int d, const char *format, ...);
int vprintf(const char *format, va_list ap);
int vfprintf(FILE  *stream,  const  char *format, va_list ap);
int vsprintf(char *str, const char *format, va_list ap);
int vsnprintf(char *str, size_t size, const char  *format, va_list ap);
int vasprintf(char  **strp,  const  char *format, va_list ap);
int vdprintf(int d, const char *format, va_list ap);

void perror(const char *s);

/* string.h */
size_t strlen(const char *str);
char *strcat(char *dest, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strcpy(char *dest, const char *src);
int strcmp(const char *str1, const char *str2);
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp (const void * ptr1, const void * ptr2, size_t num);
char *strdup(const char *s);

/* dlfcn.h */
#define RTLD_LAZY       0x001
#define RTLD_NOW        0x002
#define RTLD_GLOBAL     0x100

void *dlopen(const char *filename, int flag);
const char *dlerror(void);
void *dlsym(void *handle, char *symbol);
int dlclose(void *handle);


/* elf.h */
/* Intel 80386 specific definitions.  */

/* i386 relocs.  */

#define R_386_NONE	0		/* No reloc */
#define R_386_32	1		/* Direct 32 bit  */
#define R_386_PC32	2		/* PC relative 32 bit */
#define R_386_GOT32	3		/* 32 bit GOT entry */
#define R_386_PLT32	4		/* 32 bit PLT address */
#define R_386_COPY	5		/* Copy symbol at runtime */
#define R_386_GLOB_DAT	6		/* Create GOT entry */
#define R_386_JMP_SLOT	7		/* Create PLT entry */
#define R_386_RELATIVE	8		/* Adjust by program base */
#define R_386_GOTOFF	9		/* 32 bit offset to GOT */
#define R_386_GOTPC	10		/* 32 bit PC relative offset to GOT */
/* Keep this the last entry.  */
#define R_386_NUM	11

