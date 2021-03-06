/*
 *  TCC - Tiny C Compiler
 * 
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tcc.h"
#include "tccelf.h"
#include "tccpp.h"
#include "elf.h"
#include "stab.h"
#include "libtcc.h"
#ifdef __i386__
#include "i386-gen.h"
#elif defined(__x86_64__)
#include "x86_64-gen.h"
#endif

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <setjmp.h>
#include <time.h>
#ifdef _WIN32
#include <sys/timeb.h>
#include <windows.h>
#else
#include <dlfcn.h> 
#include <sys/time.h>
#include <sys/mman.h>
#ifdef CONFIG_TCC_DEBUG
#include <sys/ucontext.h>
#endif /* CONFIG_TCC_DEBUG */
#endif /* WIN32 */

#ifndef PAGESIZE
#define PAGESIZE 4096
#endif

/* parser debug */
//#define PARSE_DEBUG
/* preprocessor debug */
//#define PP_DEBUG
/* include file debug */
//#define INC_DEBUG

//#define MEM_DEBUG

static const char tcc_keywords[] = 
#define DEF(id, str) str "\0"
#include "tcctok.h"
#undef DEF
;

#define TOK_UIDENT TOK_DEFINE

#ifdef _WIN32
#define snprintf _snprintf
#define vsnprintf _vsnprintf
#ifndef __GNUC__
  #define strtold (long double)strtod
  #define strtof (float)strtod
  #define strtoll (long long)strtol
#endif
#elif defined(TCC_UCLIBC) || defined(__FreeBSD__) || defined(__DragonFly__) \
    || defined(__OpenBSD__)
/* currently incorrect */
long double strtold(const char *nptr, char **endptr)
{
    return (long double)strtod(nptr, endptr);
}
float strtof(const char *nptr, char **endptr)
{
    return (float)strtod(nptr, endptr);
}
#else
/* XXX: need to define this to use them in non ISOC99 context */
extern float strtof (const char *__nptr, char **__endptr);
extern long double strtold (const char *__nptr, char **__endptr);
#endif

static void macro_subst(TokenString *tok_str, Sym **nested_list, 
                        const int *macro_str, struct macro_level **can_read_stream);
static void parse_expr_type(CType *type);
static void expr_type(CType *type);
static void unary_type(CType *type);
static void block(int *bsym, int *csym, int *case_sym, int *def_sym, 
                  int case_reg, int is_expr);
static void expr_eq(void);
static void gen_inline_functions(void);
static void decl(int l);
static void decl_initializer(CType *type, Section *sec, unsigned long c, 
                             int first, int size_only);
static void decl_initializer_alloc(CType *type, AttributeDef *ad, int r, 
                                   int has_init, int v, int scope);
static void gen_cast(CType *type);
static Sym *sym_find(int v);

/* type handling */
static inline CType *pointed_type(CType *type);
static int pointed_size(CType *type);
static int parse_btype(CType *type, AttributeDef *ad);
static void type_decl(CType *type, AttributeDef *ad, int *v, int td);
static int compare_types(CType *type1, CType *type2, int unqualified);
static int is_compatible_types(CType *type1, CType *type2);
static int is_compatible_parameter_types(CType *type1, CType *type2);

static int tcc_add_file_internal(TCCState *s, const char *filename, int flags);

#ifdef CONFIG_TCC_STATIC

#define RTLD_LAZY       0x001
#define RTLD_NOW        0x002
#define RTLD_GLOBAL     0x100
#define RTLD_DEFAULT    NULL

/* dummy function for profiling */
void *dlopen(const char *filename, int flag)
{
    return NULL;
}

typedef struct TCCSyms {
    char *str;
    void *ptr;
} TCCSyms;

#define TCCSYM(a) { #a, &a, },

/* add the symbol you want here if no dynamic linking is done */
static TCCSyms tcc_syms[] = {
#if !defined(CONFIG_TCCBOOT)
    TCCSYM(printf)
    TCCSYM(fprintf)
    TCCSYM(fopen)
    TCCSYM(fclose)
#endif
    { NULL, NULL },
};

void *resolve_sym(TCCState *s1, const char *symbol, int type)
{
    TCCSyms *p;
    p = tcc_syms;
    while (p->str != NULL) {
        if (!strcmp(p->str, symbol))
            return p->ptr;
        p++;
    }
    return NULL;
}

#elif !defined(_WIN32)

#include <dlfcn.h>

void *resolve_sym(TCCState *s1, const char *sym, int type)
{
    return dlsym(RTLD_DEFAULT, sym);
}

#endif

/********************************************************/

/* we use our own 'finite' function to avoid potential problems with
   non standard math libs */
/* XXX: endianness dependent */
int ieee_finite(double d)
{
    int *p = (int *)&d;
    return ((unsigned)((p[1] | 0x800fffff) + 1)) >> 31;
}

/* copy a string and truncate it. */
char *pstrcpy(char *buf, int buf_size, const char *s)
{
    char *q, *q_end;
    int c;

    if (buf_size > 0) {
        q = buf;
        q_end = buf + buf_size - 1;
        while (q < q_end) {
            c = *s++;
            if (c == '\0')
                break;
            *q++ = c;
        }
        *q = '\0';
    }
    return buf;
}

/* strcat and truncate. */
char *pstrcat(char *buf, int buf_size, const char *s)
{
    int len;
    len = strlen(buf);
    if (len < buf_size) 
        pstrcpy(buf + len, buf_size - len, s);
    return buf;
}

#ifndef LIBTCC
static int strstart(const char *str, const char *val, const char **ptr)
{
    const char *p, *q;
    p = str;
    q = val;
    while (*q != '\0') {
        if (*p != *q)
            return 0;
        p++;
        q++;
    }
    if (ptr)
        *ptr = p;
    return 1;
}
#endif

/* extract the basename of a file */
char *tcc_basename(const char *name)
{
    char *p = strchr(name, 0);
    while (p > name && !IS_PATHSEP(p[-1]))
        --p;
    return p;
}

static char *tcc_fileextension (const char *name)
{
    char *b = tcc_basename(name);
    char *e = strrchr(b, '.');
    return e ? e : strchr(b, 0);
}

#ifdef _WIN32
char *normalize_slashes(char *path)
{
    char *p;
    for (p = path; *p; ++p)
        if (*p == '\\')
            *p = '/';
    return path;
}

void tcc_set_lib_path_w32(TCCState *s)
{
    /* on win32, we suppose the lib and includes are at the location
       of 'tcc.exe' */
    char path[1024], *p;
    GetModuleFileNameA(NULL, path, sizeof path);
    p = tcc_basename(normalize_slashes(strlwr(path)));
    if (p - 5 > path && 0 == strncmp(p - 5, "/bin/", 5))
        p -= 5;
    else if (p > path)
        p--;
    *p = 0;
    tcc_set_lib_path(s, path);
}
#else
char *normalize_slashes(char *path)
{   
    return path;
}
#endif

void set_pages_executable(void *ptr, unsigned long length)
{
#ifdef _WIN32
    unsigned long old_protect;
    VirtualProtect(ptr, length, PAGE_EXECUTE_READWRITE, &old_protect);
#else
    unsigned long start, end;
    start = (unsigned long)ptr & ~(PAGESIZE - 1);
    end = (unsigned long)ptr + length;
    end = (end + PAGESIZE - 1) & ~(PAGESIZE - 1);
    mprotect((void *)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif            
}

void *tcc_malloc(unsigned long size)
{
    void *ptr;
    ptr = malloc(size);
    if (!ptr && size)
        error("memory full");
#ifdef MEM_DEBUG
    mem_cur_size += malloc_usable_size(ptr);
    if (mem_cur_size > mem_max_size)
        mem_max_size = mem_cur_size;
#endif
    return ptr;
}

void *tcc_mallocz(unsigned long size)
{
    void *ptr;
    ptr = tcc_malloc(size);
    memset(ptr, 0, size);
    return ptr;
}

static char *tcc_strdup(const char *str)
{
    char *ptr;
    ptr = tcc_malloc(strlen(str) + 1);
    strcpy(ptr, str);
    return ptr;
}

#define free(p) use_tcc_free(p)
#define malloc(s) use_tcc_malloc(s)
#define realloc(p, s) use_tcc_realloc(p, s)

void dynarray_add(void ***ptab, int *nb_ptr, void *data)
{
    int nb, nb_alloc;
    void **pp;
    
    nb = *nb_ptr;
    pp = *ptab;
    /* every power of two we double array size */
    if ((nb & (nb - 1)) == 0) {
        if (!nb)
            nb_alloc = 1;
        else
            nb_alloc = nb * 2;
        pp = tcc_realloc(pp, nb_alloc * sizeof(void *));
        if (!pp)
            error("memory full");
        *ptab = pp;
    }
    pp[nb++] = data;
    *nb_ptr = nb;
}

static void dynarray_reset(void *pp, int *n)
{
    void **p;
    for (p = *(void***)pp; *n; ++p, --*n)
        if (*p)
            tcc_free(*p);
    tcc_free(*(void**)pp);
    *(void**)pp = NULL;
}

/* symbol allocator */
static Sym *__sym_malloc(void)
{
    Sym *sym_pool, *sym, *last_sym;
    int i;

    sym_pool = tcc_malloc(SYM_POOL_NB * sizeof(Sym));
    dynarray_add(&sym_pools, &nb_sym_pools, sym_pool);

    last_sym = sym_free_first;
    sym = sym_pool;
    for(i = 0; i < SYM_POOL_NB; i++) {
        sym->next = last_sym;
        last_sym = sym;
        sym++;
    }
    sym_free_first = last_sym;
    return last_sym;
}

static inline Sym *sym_malloc(void)
{
    Sym *sym;
    sym = sym_free_first;
    if (!sym)
        sym = __sym_malloc();
    sym_free_first = sym->next;
    return sym;
}

Section *new_section(TCCState *s1, const char *name, int sh_type, int sh_flags)
{
    Section *sec;

    sec = tcc_mallocz(sizeof(Section) + strlen(name));
    strcpy(sec->name, name);
    sec->sh_type = sh_type;
    sec->sh_flags = sh_flags;
    switch(sh_type) {
    case SHT_HASH:
    case SHT_REL:
    case SHT_RELA:
    case SHT_DYNSYM:
    case SHT_SYMTAB:
    case SHT_DYNAMIC:
        sec->sh_addralign = 4;
        break;
    case SHT_STRTAB:
        sec->sh_addralign = 1;
        break;
    default:
        sec->sh_addralign = 32; /* default conservative alignment */
        break;
    }

    if (sh_flags & SHF_PRIVATE) {
        dynarray_add((void ***)&s1->priv_sections, &s1->nb_priv_sections, sec);
    } else {
        sec->sh_num = s1->nb_sections;
        dynarray_add((void ***)&s1->sections, &s1->nb_sections, sec);
    }

    return sec;
}

static void free_section(Section *s)
{
    tcc_free(s->data);
}

/* realloc section and set its content to zero */
void section_realloc(Section *sec, unsigned long new_size)
{
    unsigned long size;
    unsigned char *data;
    
    size = sec->data_allocated;
    if (size == 0)
        size = 1;
    while (size < new_size)
        size = size * 2;
    data = tcc_realloc(sec->data, size);
    if (!data)
        error("memory full");
    memset(data + sec->data_allocated, 0, size - sec->data_allocated);
    sec->data = data;
    sec->data_allocated = size;
}

/* reserve at least 'size' bytes in section 'sec' from
   sec->data_offset. */
void *section_ptr_add(Section *sec, unsigned long size)
{
    unsigned long offset, offset1;

    offset = sec->data_offset;
    offset1 = offset + size;
    if (offset1 > sec->data_allocated)
        section_realloc(sec, offset1);
    sec->data_offset = offset1;
    return sec->data + offset;
}

/* return a reference to a section, and create it if it does not
   exists */
Section *find_section(TCCState *s1, const char *name)
{
    Section *sec;
    int i;
    for(i = 1; i < s1->nb_sections; i++) {
        sec = s1->sections[i];
        if (!strcmp(name, sec->name)) 
            return sec;
    }
    /* sections are created as PROGBITS */
    return new_section(s1, name, SHT_PROGBITS, SHF_ALLOC);
}

/* update sym->c so that it points to an external symbol in section
   'section' with value 'value' */
void put_extern_sym2(Sym *sym, Section *section, 
                     unsigned long value, unsigned long size,
                     int can_add_underscore)
{
    int sym_type, sym_bind, sh_num, info, other, attr;
    ElfW(Sym) *esym;
    const char *name;
    char buf1[256];

    if (section == NULL)
        sh_num = SHN_UNDEF;
    else if (section == SECTION_ABS) 
        sh_num = SHN_ABS;
    else
        sh_num = section->sh_num;

    other = attr = 0;

    if ((sym->type.t & VT_BTYPE) == VT_FUNC) {
        sym_type = STT_FUNC;
#ifdef TCC_TARGET_PE
        if (sym->type.ref)
            attr = sym->type.ref->r;
        if (FUNC_EXPORT(attr))
            other |= 1;
        if (FUNC_CALL(attr) == FUNC_STDCALL)
            other |= 2;
#endif
    } else {
        sym_type = STT_OBJECT;
    }

    if (sym->type.t & VT_STATIC)
        sym_bind = STB_LOCAL;
    else
        sym_bind = STB_GLOBAL;

    if (!sym->c) {
        name = get_tok_str(sym->v, NULL);
#ifdef CONFIG_TCC_BCHECK
        if (do_bounds_check) {
            char buf[32];

            /* XXX: avoid doing that for statics ? */
            /* if bound checking is activated, we change some function
               names by adding the "__bound" prefix */
            switch(sym->v) {
#if 0
            /* XXX: we rely only on malloc hooks */
            case TOK_malloc: 
            case TOK_free: 
            case TOK_realloc: 
            case TOK_memalign: 
            case TOK_calloc: 
#endif
            case TOK_memcpy: 
            case TOK_memmove:
            case TOK_memset:
            case TOK_strlen:
            case TOK_strcpy:
            case TOK__alloca:
                strcpy(buf, "__bound_");
                strcat(buf, name);
                name = buf;
                break;
            }
        }
#endif

#ifdef TCC_TARGET_PE
        if ((other & 2) && can_add_underscore) {
            sprintf(buf1, "_%s@%d", name, FUNC_ARGS(attr));
            name = buf1;
        } else
#endif
        if (tcc_state->leading_underscore && can_add_underscore) {
            buf1[0] = '_';
            pstrcpy(buf1 + 1, sizeof(buf1) - 1, name);
            name = buf1;
        }
        info = ELFW(ST_INFO)(sym_bind, sym_type);
        sym->c = add_elf_sym(symtab_section, value, size, info, other, sh_num, name);
    } else {
        esym = &((ElfW(Sym) *)symtab_section->data)[sym->c];
        esym->st_value = value;
        esym->st_size = size;
        esym->st_shndx = sh_num;
        esym->st_other |= other;
    }
}

void put_extern_sym(Sym *sym, Section *section, 
                    unsigned long value, unsigned long size)
{
    put_extern_sym2(sym, section, value, size, 1);
}

/* add a new relocation entry to symbol 'sym' in section 's' */
void greloc(Section *s, Sym *sym, unsigned long offset, int type)
{
    if (!sym->c) 
        put_extern_sym(sym, NULL, 0, 0);
    /* now we can add ELF relocation info */
    put_elf_reloc(symtab_section, s, offset, type, sym->c);
}

int isoct(int c)
{
    return c >= '0' && c <= '7';
}

int toup(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    else
        return c;
}

static void strcat_vprintf(char *buf, int buf_size, const char *fmt, va_list ap)
{
    int len;
    len = strlen(buf);
    vsnprintf(buf + len, buf_size - len, fmt, ap);
}

static void strcat_printf(char *buf, int buf_size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    strcat_vprintf(buf, buf_size, fmt, ap);
    va_end(ap);
}

void error1(TCCState *s1, int is_warning, const char *fmt, va_list ap)
{
    char buf[2048];
    BufferedFile **f;
    
    buf[0] = '\0';
    if (file) {
        for(f = s1->include_stack; f < s1->include_stack_ptr; f++)
            strcat_printf(buf, sizeof(buf), "In file included from %s:%d:\n", 
                          (*f)->filename, (*f)->line_num);
        if (file->line_num > 0) {
            strcat_printf(buf, sizeof(buf), 
                          "%s:%d: ", file->filename, file->line_num);
        } else {
            strcat_printf(buf, sizeof(buf),
                          "%s: ", file->filename);
        }
    } else {
        strcat_printf(buf, sizeof(buf),
                      "tcc: ");
    }
    if (is_warning)
        strcat_printf(buf, sizeof(buf), "warning: ");
    strcat_vprintf(buf, sizeof(buf), fmt, ap);

    if (!s1->error_func) {
        /* default case: stderr */
        fprintf(stderr, "%s\n", buf);
    } else {
        s1->error_func(s1->error_opaque, buf);
    }
    if (!is_warning || s1->warn_error)
        s1->nb_errors++;
}

#ifdef LIBTCC
void tcc_set_error_func(TCCState *s, void *error_opaque,
                        void (*error_func)(void *opaque, const char *msg))
{
    s->error_opaque = error_opaque;
    s->error_func = error_func;
}
#endif

/* error without aborting current compilation */
void error_noabort(const char *fmt, ...)
{
    TCCState *s1 = tcc_state;
    va_list ap;

    va_start(ap, fmt);
    error1(s1, 0, fmt, ap);
    va_end(ap);
}

void error(const char *fmt, ...)
{
    TCCState *s1 = tcc_state;
    va_list ap;

    va_start(ap, fmt);
    error1(s1, 0, fmt, ap);
    va_end(ap);
    /* better than nothing: in some cases, we accept to handle errors */
    if (s1->error_set_jmp_enabled) {
        longjmp(s1->error_jmp_buf, 1);
    } else {
        /* XXX: eliminate this someday */
        exit(1);
    }
}

void expect(const char *msg)
{
    error("%s expected", msg);
}

void warning(const char *fmt, ...)
{
    TCCState *s1 = tcc_state;
    va_list ap;

    if (s1->warn_none)
        return;

    va_start(ap, fmt);
    error1(s1, 1, fmt, ap);
    va_end(ap);
}

void skip(int c)
{
    if (tok != c)
        error("'%c' expected", c);
    next();
}

void test_lvalue(void)
{
    if (!(vtop->r & VT_LVAL))
        expect("lvalue");
}

/* allocate a new token */
TokenSym *tok_alloc_new(TokenSym **pts, const char *str, int len)
{
    TokenSym *ts, **ptable;
    int i;

    if (tok_ident >= SYM_FIRST_ANOM) 
        error("memory full");

    /* expand token table if needed */
    i = tok_ident - TOK_IDENT;
    if ((i % TOK_ALLOC_INCR) == 0) {
        ptable = tcc_realloc(table_ident, (i + TOK_ALLOC_INCR) * sizeof(TokenSym *));
        if (!ptable)
            error("memory full");
        table_ident = ptable;
    }

    ts = tcc_malloc(sizeof(TokenSym) + len);
    table_ident[i] = ts;
    ts->tok = tok_ident++;
    ts->sym_define = NULL;
    ts->sym_label = NULL;
    ts->sym_struct = NULL;
    ts->sym_identifier = NULL;
    ts->len = len;
    ts->hash_next = NULL;
    memcpy(ts->str, str, len);
    ts->str[len] = '\0';
    *pts = ts;
    return ts;
}

/* find a token and add it if not found */
TokenSym *tok_alloc(const char *str, int len)
{
    TokenSym *ts, **pts;
    int i;
    unsigned int h;
    
    h = TOK_HASH_INIT;
    for(i=0;i<len;i++)
        h = TOK_HASH_FUNC(h, ((unsigned char *)str)[i]);
    h &= (TOK_HASH_SIZE - 1);

    pts = &hash_ident[h];
    for(;;) {
        ts = *pts;
        if (!ts)
            break;
        if (ts->len == len && !memcmp(ts->str, str, len))
            return ts;
        pts = &(ts->hash_next);
    }
    return tok_alloc_new(pts, str, len);
}

/* CString handling */

static void cstr_realloc(CString *cstr, int new_size)
{
    int size;
    void *data;

    size = cstr->size_allocated;
    if (size == 0)
        size = 8; /* no need to allocate a too small first string */
    while (size < new_size)
        size = size * 2;
    data = tcc_realloc(cstr->data_allocated, size);
    if (!data)
        error("memory full");
    cstr->data_allocated = data;
    cstr->size_allocated = size;
    cstr->data = data;
}

/* add a byte */
void cstr_ccat(CString *cstr, int ch)
{
    int size;
    size = cstr->size + 1;
    if (size > cstr->size_allocated)
        cstr_realloc(cstr, size);
    ((unsigned char *)cstr->data)[size - 1] = ch;
    cstr->size = size;
}

void cstr_cat(CString *cstr, const char *str)
{
    int c;
    for(;;) {
        c = *str;
        if (c == '\0')
            break;
        cstr_ccat(cstr, c);
        str++;
    }
}

/* add a wide char */
void cstr_wccat(CString *cstr, int ch)
{
    int size;
    size = cstr->size + sizeof(nwchar_t);
    if (size > cstr->size_allocated)
        cstr_realloc(cstr, size);
    *(nwchar_t *)(((unsigned char *)cstr->data) + size - sizeof(nwchar_t)) = ch;
    cstr->size = size;
}

void cstr_new(CString *cstr)
{
    memset(cstr, 0, sizeof(CString));
}

/* free string and reset it to NULL */
void cstr_free(CString *cstr)
{
    tcc_free(cstr->data_allocated);
    cstr_new(cstr);
}

/* XXX: unicode ? */
static void add_char(CString *cstr, int c)
{
    if (c == '\'' || c == '\"' || c == '\\') {
        /* XXX: could be more precise if char or string */
        cstr_ccat(cstr, '\\');
    }
    if (c >= 32 && c <= 126) {
        cstr_ccat(cstr, c);
    } else {
        cstr_ccat(cstr, '\\');
        if (c == '\n') {
            cstr_ccat(cstr, 'n');
        } else {
            cstr_ccat(cstr, '0' + ((c >> 6) & 7));
            cstr_ccat(cstr, '0' + ((c >> 3) & 7));
            cstr_ccat(cstr, '0' + (c & 7));
        }
    }
}

/* XXX: buffer overflow */
/* XXX: float tokens */
char *get_tok_str(int v, CValue *cv)
{
    static char buf[STRING_MAX_SIZE + 1];
    static CString cstr_buf;
    CString *cstr;
    unsigned char *q;
    char *p;
    int i, len;

    /* NOTE: to go faster, we give a fixed buffer for small strings */
    cstr_reset(&cstr_buf);
    cstr_buf.data = buf;
    cstr_buf.size_allocated = sizeof(buf);
    p = buf;

    switch(v) {
    case TOK_CINT:
    case TOK_CUINT:
        /* XXX: not quite exact, but only useful for testing */
        sprintf(p, "%u", cv->ui);
        break;
    case TOK_CLLONG:
    case TOK_CULLONG:
        /* XXX: not quite exact, but only useful for testing  */
        sprintf(p, "%Lu", cv->ull);
        break;
    case TOK_LCHAR:
	cstr_ccat(&cstr_buf, 'L');
    case TOK_CCHAR:
        cstr_ccat(&cstr_buf, '\'');
        add_char(&cstr_buf, cv->i);
        cstr_ccat(&cstr_buf, '\'');
        cstr_ccat(&cstr_buf, '\0');
        break;
    case TOK_PPNUM:
        cstr = cv->cstr;
        len = cstr->size - 1;
        for(i=0;i<len;i++)
            add_char(&cstr_buf, ((unsigned char *)cstr->data)[i]);
        cstr_ccat(&cstr_buf, '\0');
        break;
    case TOK_LSTR:
	cstr_ccat(&cstr_buf, 'L');
    case TOK_STR:
        cstr = cv->cstr;
        cstr_ccat(&cstr_buf, '\"');
        if (v == TOK_STR) {
            len = cstr->size - 1;
            for(i=0;i<len;i++)
                add_char(&cstr_buf, ((unsigned char *)cstr->data)[i]);
        } else {
            len = (cstr->size / sizeof(nwchar_t)) - 1;
            for(i=0;i<len;i++)
                add_char(&cstr_buf, ((nwchar_t *)cstr->data)[i]);
        }
        cstr_ccat(&cstr_buf, '\"');
        cstr_ccat(&cstr_buf, '\0');
        break;
    case TOK_LT:
        v = '<';
        goto addv;
    case TOK_GT:
        v = '>';
        goto addv;
    case TOK_DOTS:
        return strcpy(p, "...");
    case TOK_A_SHL:
        return strcpy(p, "<<=");
    case TOK_A_SAR:
        return strcpy(p, ">>=");
    default:
        if (v < TOK_IDENT) {
            /* search in two bytes table */
            q = tok_two_chars;
            while (*q) {
                if (q[2] == v) {
                    *p++ = q[0];
                    *p++ = q[1];
                    *p = '\0';
                    return buf;
                }
                q += 3;
            }
        addv:
            *p++ = v;
            *p = '\0';
        } else if (v < tok_ident) {
            return table_ident[v - TOK_IDENT]->str;
        } else if (v >= SYM_FIRST_ANOM) {
            /* special name for anonymous symbol */
            sprintf(p, "L.%u", v - SYM_FIRST_ANOM);
        } else {
            /* should never happen */
            return NULL;
        }
        break;
    }
    return cstr_buf.data;
}

/* push, without hashing */
Sym *sym_push2(Sym **ps, int v, int t, long c)
{
    Sym *s;
    s = sym_malloc();
    s->v = v;
    s->type.t = t;
    s->c = c;
    s->next = NULL;
    /* add in stack */
    s->prev = *ps;
    *ps = s;
    return s;
}

/* find a symbol and return its associated structure. 's' is the top
   of the symbol stack */
Sym *sym_find2(Sym *s, int v)
{
    while (s) {
        if (s->v == v)
            return s;
        s = s->prev;
    }
    return NULL;
}

/* structure lookup */
static inline Sym *struct_find(int v)
{
    v -= TOK_IDENT;
    if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
        return NULL;
    return table_ident[v]->sym_struct;
}

/* find an identifier */
static inline Sym *sym_find(int v)
{
    v -= TOK_IDENT;
    if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
        return NULL;
    return table_ident[v]->sym_identifier;
}

/* push a given symbol on the symbol stack */
Sym *sym_push(int v, CType *type, int r, int c)
{
    Sym *s, **ps;
    TokenSym *ts;

    if (local_stack)
        ps = &local_stack;
    else
        ps = &global_stack;
    s = sym_push2(ps, v, type->t, c);
    s->type.ref = type->ref;
    s->r = r;
    /* don't record fields or anonymous symbols */
    /* XXX: simplify */
    if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM) {
        /* record symbol in token array */
        ts = table_ident[(v & ~SYM_STRUCT) - TOK_IDENT];
        if (v & SYM_STRUCT)
            ps = &ts->sym_struct;
        else
            ps = &ts->sym_identifier;
        s->prev_tok = *ps;
        *ps = s;
    }
    return s;
}

/* push a global identifier */
static Sym *global_identifier_push(int v, int t, int c)
{
    Sym *s, **ps;
    s = sym_push2(&global_stack, v, t, c);
    /* don't record anonymous symbol */
    if (v < SYM_FIRST_ANOM) {
        ps = &table_ident[v - TOK_IDENT]->sym_identifier;
        /* modify the top most local identifier, so that
           sym_identifier will point to 's' when popped */
        while (*ps != NULL)
            ps = &(*ps)->prev_tok;
        s->prev_tok = NULL;
        *ps = s;
    }
    return s;
}

/* pop symbols until top reaches 'b' */
static void sym_pop(Sym **ptop, Sym *b)
{
    Sym *s, *ss, **ps;
    TokenSym *ts;
    int v;

    s = *ptop;
    while(s != b) {
        ss = s->prev;
        v = s->v;
        /* remove symbol in token array */
        /* XXX: simplify */
        if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM) {
            ts = table_ident[(v & ~SYM_STRUCT) - TOK_IDENT];
            if (v & SYM_STRUCT)
                ps = &ts->sym_struct;
            else
                ps = &ts->sym_identifier;
            *ps = s->prev_tok;
        }
        sym_free(s);
        s = ss;
    }
    *ptop = b;
}

/* I/O layer */

BufferedFile *tcc_open(TCCState *s1, const char *filename)
{
    int fd;
    BufferedFile *bf;

    if (strcmp(filename, "-") == 0)
        fd = 0, filename = "stdin";
    else
        fd = open(filename, O_RDONLY | O_BINARY);
    if ((verbose == 2 && fd >= 0) || verbose == 3)
        printf("%s %*s%s\n", fd < 0 ? "nf":"->",
               (s1->include_stack_ptr - s1->include_stack), "", filename);
    if (fd < 0)
        return NULL;
    bf = tcc_malloc(sizeof(BufferedFile));
    bf->fd = fd;
    bf->buf_ptr = bf->buffer;
    bf->buf_end = bf->buffer;
    bf->buffer[0] = CH_EOB; /* put eob symbol */
    pstrcpy(bf->filename, sizeof(bf->filename), filename);
#ifdef _WIN32
    normalize_slashes(bf->filename);
#endif
    bf->line_num = 1;
    bf->ifndef_macro = 0;
    bf->ifdef_stack_ptr = s1->ifdef_stack_ptr;
    //    printf("opening '%s'\n", filename);
    return bf;
}

void tcc_close(BufferedFile *bf)
{
    total_lines += bf->line_num;
    close(bf->fd);
    tcc_free(bf);
}

void swap(int *p, int *q)
{
    int t;
    t = *p;
    *p = *q;
    *q = t;
}

void vsetc(CType *type, int r, CValue *vc)
{
    int v;

    if (vtop >= vstack + (VSTACK_SIZE - 1))
        error("memory full");
    /* cannot let cpu flags if other instruction are generated. Also
       avoid leaving VT_JMP anywhere except on the top of the stack
       because it would complicate the code generator. */
    if (vtop >= vstack) {
        v = vtop->r & VT_VALMASK;
        if (v == VT_CMP || (v & ~1) == VT_JMP)
            gv(RC_INT);
    }
    vtop++;
    vtop->type = *type;
    vtop->r = r;
    vtop->r2 = VT_CONST;
    vtop->c = *vc;
}

/* push integer constant */
void vpushi(int v)
{
    CValue cval;
    cval.i = v;
    vsetc(&int_type, VT_CONST, &cval);
}

/* push long long constant */
void vpushll(long long v)
{
    CValue cval;
    CType ctype;
    ctype.t = VT_LLONG;
    cval.ull = v;
    vsetc(&ctype, VT_CONST, &cval);
}

/* Return a static symbol pointing to a section */
static Sym *get_sym_ref(CType *type, Section *sec, 
                        unsigned long offset, unsigned long size)
{
    int v;
    Sym *sym;

    v = anon_sym++;
    sym = global_identifier_push(v, type->t | VT_STATIC, 0);
    sym->type.ref = type->ref;
    sym->r = VT_CONST | VT_SYM;
    put_extern_sym(sym, sec, offset, size);
    return sym;
}

/* push a reference to a section offset by adding a dummy symbol */
static void vpush_ref(CType *type, Section *sec, unsigned long offset, unsigned long size)
{
    CValue cval;

    cval.ul = 0;
    vsetc(type, VT_CONST | VT_SYM, &cval);
    vtop->sym = get_sym_ref(type, sec, offset, size);
}

/* define a new external reference to a symbol 'v' of type 'u' */
Sym *external_global_sym(int v, CType *type, int r)
{
    Sym *s;

    s = sym_find(v);
    if (!s) {
        /* push forward reference */
        s = global_identifier_push(v, type->t | VT_EXTERN, 0);
        s->type.ref = type->ref;
        s->r = r | VT_CONST | VT_SYM;
    }
    return s;
}

/* define a new external reference to a symbol 'v' of type 'u' */
Sym *external_sym(int v, CType *type, int r)
{
    Sym *s;

    s = sym_find(v);
    if (!s) {
        /* push forward reference */
        s = sym_push(v, type, r | VT_CONST | VT_SYM, 0);
        s->type.t |= VT_EXTERN;
    } else {
        if (!is_compatible_types(&s->type, type))
            error("incompatible types for redefinition of '%s'", 
                  get_tok_str(v, NULL));
    }
    return s;
}

/* push a reference to global symbol v */
static void vpush_global_sym(CType *type, int v)
{
    Sym *sym;
    CValue cval;

    sym = external_global_sym(v, type, 0);
    cval.ul = 0;
    vsetc(type, VT_CONST | VT_SYM, &cval);
    vtop->sym = sym;
}

void vset(CType *type, int r, int v)
{
    CValue cval;

    cval.i = v;
    vsetc(type, r, &cval);
}

void vseti(int r, int v)
{
    CType type;
    type.t = VT_INT;
    vset(&type, r, v);
}

void vswap(void)
{
    SValue tmp;

    tmp = vtop[0];
    vtop[0] = vtop[-1];
    vtop[-1] = tmp;
}

void vpushv(SValue *v)
{
    if (vtop >= vstack + (VSTACK_SIZE - 1))
        error("memory full");
    vtop++;
    *vtop = *v;
}

void vdup(void)
{
    vpushv(vtop);
}

/* save r to the memory stack, and mark it as being free */
void save_reg(int r)
{
    int l, saved, size, align;
    SValue *p, sv;
    CType *type;

    /* modify all stack values */
    saved = 0;
    l = 0;
    for(p=vstack;p<=vtop;p++) {
        if ((p->r & VT_VALMASK) == r ||
            ((p->type.t & VT_BTYPE) == VT_LLONG && (p->r2 & VT_VALMASK) == r)) {
            /* must save value on stack if not already done */
            if (!saved) {
                /* NOTE: must reload 'r' because r might be equal to r2 */
                r = p->r & VT_VALMASK;
                /* store register in the stack */
                type = &p->type;
                if ((p->r & VT_LVAL) ||
                    (!is_float(type->t) && (type->t & VT_BTYPE) != VT_LLONG))
#ifdef TCC_TARGET_X86_64
                    type = &char_pointer_type;
#else
                    type = &int_type;
#endif
                size = type_size(type, &align);
                loc = (loc - size) & -align;
                sv.type.t = type->t;
                sv.r = VT_LOCAL | VT_LVAL;
                sv.c.ul = loc;
                store(r, &sv);
#if defined(TCC_TARGET_I386) || defined(TCC_TARGET_X86_64)
                /* x86 specific: need to pop fp register ST0 if saved */
                if (r == TREG_ST0) {
                    o(0xd9dd); /* fstp %st(1) */
                }
#endif
#ifndef TCC_TARGET_X86_64
                /* special long long case */
                if ((type->t & VT_BTYPE) == VT_LLONG) {
                    sv.c.ul += 4;
                    store(p->r2, &sv);
                }
#endif
                l = loc;
                saved = 1;
            }
            /* mark that stack entry as being saved on the stack */
            if (p->r & VT_LVAL) {
                /* also clear the bounded flag because the
                   relocation address of the function was stored in
                   p->c.ul */
                p->r = (p->r & ~(VT_VALMASK | VT_BOUNDED)) | VT_LLOCAL;
            } else {
                p->r = lvalue_type(p->type.t) | VT_LOCAL;
            }
            p->r2 = VT_CONST;
            p->c.ul = l;
        }
    }
}

/* find a register of class 'rc2' with at most one reference on stack.
 * If none, call get_reg(rc) */
int get_reg_ex(int rc, int rc2) 
{
    int r;
    SValue *p;
    
    for(r=0;r<NB_REGS;r++) {
        if (reg_classes[r] & rc2) {
            int n;
            n=0;
            for(p = vstack; p <= vtop; p++) {
                if ((p->r & VT_VALMASK) == r ||
                    (p->r2 & VT_VALMASK) == r)
                    n++;
            }
            if (n <= 1)
                return r;
        }
    }
    return get_reg(rc);
}

/* find a free register of class 'rc'. If none, save one register */
int get_reg(int rc)
{
    int r;
    SValue *p;

    /* find a free register */
    for(r=0;r<NB_REGS;r++) {
        if (reg_classes[r] & rc) {
            for(p=vstack;p<=vtop;p++) {
                if ((p->r & VT_VALMASK) == r ||
                    (p->r2 & VT_VALMASK) == r)
                    goto notfound;
            }
            return r;
        }
    notfound: ;
    }
    
    /* no register left : free the first one on the stack (VERY
       IMPORTANT to start from the bottom to ensure that we don't
       spill registers used in gen_opi()) */
    for(p=vstack;p<=vtop;p++) {
        r = p->r & VT_VALMASK;
        if (r < VT_CONST && (reg_classes[r] & rc))
            goto save_found;
        /* also look at second register (if long long) */
        r = p->r2 & VT_VALMASK;
        if (r < VT_CONST && (reg_classes[r] & rc)) {
        save_found:
            save_reg(r);
            return r;
        }
    }
    /* Should never comes here */
    return -1;
}

/* save registers up to (vtop - n) stack entry */
void save_regs(int n)
{
    int r;
    SValue *p, *p1;
    p1 = vtop - n;
    for(p = vstack;p <= p1; p++) {
        r = p->r & VT_VALMASK;
        if (r < VT_CONST) {
            save_reg(r);
        }
    }
}

/* move register 's' to 'r', and flush previous value of r to memory
   if needed */
void move_reg(int r, int s)
{
    SValue sv;

    if (r != s) {
        save_reg(r);
        sv.type.t = VT_INT;
        sv.r = s;
        sv.c.ul = 0;
        load(r, &sv);
    }
}

/* get address of vtop (vtop MUST BE an lvalue) */
void gaddrof(void)
{
    vtop->r &= ~VT_LVAL;
    /* tricky: if saved lvalue, then we can go back to lvalue */
    if ((vtop->r & VT_VALMASK) == VT_LLOCAL)
        vtop->r = (vtop->r & ~(VT_VALMASK | VT_LVAL_TYPE)) | VT_LOCAL | VT_LVAL;
}

#ifdef CONFIG_TCC_BCHECK
/* generate lvalue bound code */
void gbound(void)
{
    int lval_type;
    CType type1;

    vtop->r &= ~VT_MUSTBOUND;
    /* if lvalue, then use checking code before dereferencing */
    if (vtop->r & VT_LVAL) {
        /* if not VT_BOUNDED value, then make one */
        if (!(vtop->r & VT_BOUNDED)) {
            lval_type = vtop->r & (VT_LVAL_TYPE | VT_LVAL);
            /* must save type because we must set it to int to get pointer */
            type1 = vtop->type;
            vtop->type.t = VT_INT;
            gaddrof();
            vpushi(0);
            gen_bounded_ptr_add();
            vtop->r |= lval_type;
            vtop->type = type1;
        }
        /* then check for dereferencing */
        gen_bounded_ptr_deref();
    }
}
#endif

/* store vtop a register belonging to class 'rc'. lvalues are
   converted to values. Cannot be used if cannot be converted to
   register value (such as structures). */
int gv(int rc)
{
    int r, rc2, bit_pos, bit_size, size, align, i;

    /* NOTE: get_reg can modify vstack[] */
    if (vtop->type.t & VT_BITFIELD) {
        CType type;
        int bits = 32;
        bit_pos = (vtop->type.t >> VT_STRUCT_SHIFT) & 0x3f;
        bit_size = (vtop->type.t >> (VT_STRUCT_SHIFT + 6)) & 0x3f;
        /* remove bit field info to avoid loops */
        vtop->type.t &= ~(VT_BITFIELD | (-1 << VT_STRUCT_SHIFT));
        /* cast to int to propagate signedness in following ops */
        if ((vtop->type.t & VT_BTYPE) == VT_LLONG) {
            type.t = VT_LLONG;
            bits = 64;
        } else
            type.t = VT_INT;
        if((vtop->type.t & VT_UNSIGNED) ||
           (vtop->type.t & VT_BTYPE) == VT_BOOL)
            type.t |= VT_UNSIGNED;
        gen_cast(&type);
        /* generate shifts */
        vpushi(bits - (bit_pos + bit_size));
        gen_op(TOK_SHL);
        vpushi(bits - bit_size);
        /* NOTE: transformed to SHR if unsigned */
        gen_op(TOK_SAR);
        r = gv(rc);
    } else {
        if (is_float(vtop->type.t) && 
            (vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {
            Sym *sym;
            int *ptr;
            unsigned long offset;
#if defined(TCC_TARGET_ARM) && !defined(TCC_ARM_VFP)
            CValue check;
#endif
            
            /* XXX: unify with initializers handling ? */
            /* CPUs usually cannot use float constants, so we store them
               generically in data segment */
            size = type_size(&vtop->type, &align);
            offset = (data_section->data_offset + align - 1) & -align;
            data_section->data_offset = offset;
            /* XXX: not portable yet */
#if defined(__i386__) || defined(__x86_64__)
            /* Zero pad x87 tenbyte long doubles */
            if (size == LDOUBLE_SIZE)
                vtop->c.tab[2] &= 0xffff;
#endif
            ptr = section_ptr_add(data_section, size);
            size = size >> 2;
#if defined(TCC_TARGET_ARM) && !defined(TCC_ARM_VFP)
            check.d = 1;
            if(check.tab[0])
                for(i=0;i<size;i++)
                    ptr[i] = vtop->c.tab[size-1-i];
            else
#endif
            for(i=0;i<size;i++)
                ptr[i] = vtop->c.tab[i];
            sym = get_sym_ref(&vtop->type, data_section, offset, size << 2);
            vtop->r |= VT_LVAL | VT_SYM;
            vtop->sym = sym;
            vtop->c.ul = 0;
        }
#ifdef CONFIG_TCC_BCHECK
        if (vtop->r & VT_MUSTBOUND) 
            gbound();
#endif

        r = vtop->r & VT_VALMASK;
        rc2 = RC_INT;
        if (rc == RC_IRET)
            rc2 = RC_LRET;
        /* need to reload if:
           - constant
           - lvalue (need to dereference pointer)
           - already a register, but not in the right class */
        if (r >= VT_CONST || 
            (vtop->r & VT_LVAL) ||
            !(reg_classes[r] & rc) ||
            ((vtop->type.t & VT_BTYPE) == VT_LLONG && 
             !(reg_classes[vtop->r2] & rc2))) {
            r = get_reg(rc);
#ifndef TCC_TARGET_X86_64
            if ((vtop->type.t & VT_BTYPE) == VT_LLONG) {
                int r2;
                unsigned long long ll;
                /* two register type load : expand to two words
                   temporarily */
                if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {
                    /* load constant */
                    ll = vtop->c.ull;
                    vtop->c.ui = ll; /* first word */
                    load(r, vtop);
                    vtop->r = r; /* save register value */
                    vpushi(ll >> 32); /* second word */
                } else if (r >= VT_CONST || /* XXX: test to VT_CONST incorrect ? */
                           (vtop->r & VT_LVAL)) {
                    /* We do not want to modifier the long long
                       pointer here, so the safest (and less
                       efficient) is to save all the other registers
                       in the stack. XXX: totally inefficient. */
                    save_regs(1);
                    /* load from memory */
                    load(r, vtop);
                    vdup();
                    vtop[-1].r = r; /* save register value */
                    /* increment pointer to get second word */
                    vtop->type.t = VT_INT;
                    gaddrof();
                    vpushi(4);
                    gen_op('+');
                    vtop->r |= VT_LVAL;
                } else {
                    /* move registers */
                    load(r, vtop);
                    vdup();
                    vtop[-1].r = r; /* save register value */
                    vtop->r = vtop[-1].r2;
                }
                /* allocate second register */
                r2 = get_reg(rc2);
                load(r2, vtop);
                vpop();
                /* write second register */
                vtop->r2 = r2;
            } else
#endif
            if ((vtop->r & VT_LVAL) && !is_float(vtop->type.t)) {
                int t1, t;
                /* lvalue of scalar type : need to use lvalue type
                   because of possible cast */
                t = vtop->type.t;
                t1 = t;
                /* compute memory access type */
                if (vtop->r & VT_LVAL_BYTE)
                    t = VT_BYTE;
                else if (vtop->r & VT_LVAL_SHORT)
                    t = VT_SHORT;
                if (vtop->r & VT_LVAL_UNSIGNED)
                    t |= VT_UNSIGNED;
                vtop->type.t = t;
                load(r, vtop);
                /* restore wanted type */
                vtop->type.t = t1;
            } else {
                /* one register type load */
                load(r, vtop);
            }
        }
        vtop->r = r;
#ifdef TCC_TARGET_C67
        /* uses register pairs for doubles */
        if ((vtop->type.t & VT_BTYPE) == VT_DOUBLE) 
            vtop->r2 = r+1;
#endif
    }
    return r;
}

/* generate vtop[-1] and vtop[0] in resp. classes rc1 and rc2 */
void gv2(int rc1, int rc2)
{
    int v;

    /* generate more generic register first. But VT_JMP or VT_CMP
       values must be generated first in all cases to avoid possible
       reload errors */
    v = vtop[0].r & VT_VALMASK;
    if (v != VT_CMP && (v & ~1) != VT_JMP && rc1 <= rc2) {
        vswap();
        gv(rc1);
        vswap();
        gv(rc2);
        /* test if reload is needed for first register */
        if ((vtop[-1].r & VT_VALMASK) >= VT_CONST) {
            vswap();
            gv(rc1);
            vswap();
        }
    } else {
        gv(rc2);
        vswap();
        gv(rc1);
        vswap();
        /* test if reload is needed for first register */
        if ((vtop[0].r & VT_VALMASK) >= VT_CONST) {
            gv(rc2);
        }
    }
}

/* wrapper around RC_FRET to return a register by type */
int rc_fret(int t)
{
#ifdef TCC_TARGET_X86_64
    if (t == VT_LDOUBLE) {
        return RC_ST0;
    }
#endif
    return RC_FRET;
}

/* wrapper around REG_FRET to return a register by type */
int reg_fret(int t)
{
#ifdef TCC_TARGET_X86_64
    if (t == VT_LDOUBLE) {
        return TREG_ST0;
    }
#endif
    return REG_FRET;
}

/* expand long long on stack in two int registers */
void lexpand(void)
{
    int u;

    u = vtop->type.t & VT_UNSIGNED;
    gv(RC_INT);
    vdup();
    vtop[0].r = vtop[-1].r2;
    vtop[0].r2 = VT_CONST;
    vtop[-1].r2 = VT_CONST;
    vtop[0].type.t = VT_INT | u;
    vtop[-1].type.t = VT_INT | u;
}

#ifdef TCC_TARGET_ARM
/* expand long long on stack */
void lexpand_nr(void)
{
    int u,v;

    u = vtop->type.t & VT_UNSIGNED;
    vdup();
    vtop->r2 = VT_CONST;
    vtop->type.t = VT_INT | u;
    v=vtop[-1].r & (VT_VALMASK | VT_LVAL);
    if (v == VT_CONST) {
      vtop[-1].c.ui = vtop->c.ull;
      vtop->c.ui = vtop->c.ull >> 32;
      vtop->r = VT_CONST;
    } else if (v == (VT_LVAL|VT_CONST) || v == (VT_LVAL|VT_LOCAL)) {
      vtop->c.ui += 4;
      vtop->r = vtop[-1].r;
    } else if (v > VT_CONST) {
      vtop--;
      lexpand();
    } else
      vtop->r = vtop[-1].r2;
    vtop[-1].r2 = VT_CONST;
    vtop[-1].type.t = VT_INT | u;
}
#endif

/* build a long long from two ints */
void lbuild(int t)
{
    gv2(RC_INT, RC_INT);
    vtop[-1].r2 = vtop[0].r;
    vtop[-1].type.t = t;
    vpop();
}

/* rotate n first stack elements to the bottom 
   I1 ... In -> I2 ... In I1 [top is right]
*/
void vrotb(int n)
{
    int i;
    SValue tmp;

    tmp = vtop[-n + 1];
    for(i=-n+1;i!=0;i++)
        vtop[i] = vtop[i+1];
    vtop[0] = tmp;
}

/* rotate n first stack elements to the top 
   I1 ... In -> In I1 ... I(n-1)  [top is right]
 */
void vrott(int n)
{
    int i;
    SValue tmp;

    tmp = vtop[0];
    for(i = 0;i < n - 1; i++)
        vtop[-i] = vtop[-i - 1];
    vtop[-n + 1] = tmp;
}

#ifdef TCC_TARGET_ARM
/* like vrott but in other direction
   In ... I1 -> I(n-1) ... I1 In  [top is right]
 */
void vnrott(int n)
{
    int i;
    SValue tmp;

    tmp = vtop[-n + 1];
    for(i = n - 1; i > 0; i--)
        vtop[-i] = vtop[-i + 1];
    vtop[0] = tmp;
}
#endif

/* pop stack value */
void vpop(void)
{
    int v;
    v = vtop->r & VT_VALMASK;
#if defined(TCC_TARGET_I386) || defined(TCC_TARGET_X86_64)
    /* for x86, we need to pop the FP stack */
    if (v == TREG_ST0 && !nocode_wanted) {
        o(0xd9dd); /* fstp %st(1) */
    } else
#endif
    if (v == VT_JMP || v == VT_JMPI) {
        /* need to put correct jump if && or || without test */
        gsym(vtop->c.ul);
    }
    vtop--;
}

/* convert stack entry to register and duplicate its value in another
   register */
void gv_dup(void)
{
    int rc, t, r, r1;
    SValue sv;

    t = vtop->type.t;
    if ((t & VT_BTYPE) == VT_LLONG) {
        lexpand();
        gv_dup();
        vswap();
        vrotb(3);
        gv_dup();
        vrotb(4);
        /* stack: H L L1 H1 */
        lbuild(t);
        vrotb(3);
        vrotb(3);
        vswap();
        lbuild(t);
        vswap();
    } else {
        /* duplicate value */
        rc = RC_INT;
        sv.type.t = VT_INT;
        if (is_float(t)) {
            rc = RC_FLOAT;
#ifdef TCC_TARGET_X86_64
            if ((t & VT_BTYPE) == VT_LDOUBLE) {
                rc = RC_ST0;
            }
#endif
            sv.type.t = t;
        }
        r = gv(rc);
        r1 = get_reg(rc);
        sv.r = r;
        sv.c.ul = 0;
        load(r1, &sv); /* move r to r1 */
        vdup();
        /* duplicates value */
        vtop->r = r1;
    }
}

#ifndef TCC_TARGET_X86_64
/* generate CPU independent (unsigned) long long operations */
void gen_opl(int op)
{
    int t, a, b, op1, c, i;
    int func;
    unsigned short reg_iret = REG_IRET;
    unsigned short reg_lret = REG_LRET;
    SValue tmp;

    switch(op) {
    case '/':
    case TOK_PDIV:
        func = TOK___divdi3;
        goto gen_func;
    case TOK_UDIV:
        func = TOK___udivdi3;
        goto gen_func;
    case '%':
        func = TOK___moddi3;
        goto gen_mod_func;
    case TOK_UMOD:
        func = TOK___umoddi3;
    gen_mod_func:
#ifdef TCC_ARM_EABI
	reg_iret = TREG_R2;
	reg_lret = TREG_R3;
#endif
    gen_func:
        /* call generic long long function */
        vpush_global_sym(&func_old_type, func);
        vrott(3);
        gfunc_call(2);
        vpushi(0);
        vtop->r = reg_iret;
        vtop->r2 = reg_lret;
        break;
    case '^':
    case '&':
    case '|':
    case '*':
    case '+':
    case '-':
        t = vtop->type.t;
        vswap();
        lexpand();
        vrotb(3);
        lexpand();
        /* stack: L1 H1 L2 H2 */
        tmp = vtop[0];
        vtop[0] = vtop[-3];
        vtop[-3] = tmp;
        tmp = vtop[-2];
        vtop[-2] = vtop[-3];
        vtop[-3] = tmp;
        vswap();
        /* stack: H1 H2 L1 L2 */
        if (op == '*') {
            vpushv(vtop - 1);
            vpushv(vtop - 1);
            gen_op(TOK_UMULL);
            lexpand();
            /* stack: H1 H2 L1 L2 ML MH */
            for(i=0;i<4;i++)
                vrotb(6);
            /* stack: ML MH H1 H2 L1 L2 */
            tmp = vtop[0];
            vtop[0] = vtop[-2];
            vtop[-2] = tmp;
            /* stack: ML MH H1 L2 H2 L1 */
            gen_op('*');
            vrotb(3);
            vrotb(3);
            gen_op('*');
            /* stack: ML MH M1 M2 */
            gen_op('+');
            gen_op('+');
        } else if (op == '+' || op == '-') {
            /* XXX: add non carry method too (for MIPS or alpha) */
            if (op == '+')
                op1 = TOK_ADDC1;
            else
                op1 = TOK_SUBC1;
            gen_op(op1);
            /* stack: H1 H2 (L1 op L2) */
            vrotb(3);
            vrotb(3);
            gen_op(op1 + 1); /* TOK_xxxC2 */
        } else {
            gen_op(op);
            /* stack: H1 H2 (L1 op L2) */
            vrotb(3);
            vrotb(3);
            /* stack: (L1 op L2) H1 H2 */
            gen_op(op);
            /* stack: (L1 op L2) (H1 op H2) */
        }
        /* stack: L H */
        lbuild(t);
        break;
    case TOK_SAR:
    case TOK_SHR:
    case TOK_SHL:
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
            t = vtop[-1].type.t;
            vswap();
            lexpand();
            vrotb(3);
            /* stack: L H shift */
            c = (int)vtop->c.i;
            /* constant: simpler */
            /* NOTE: all comments are for SHL. the other cases are
               done by swaping words */
            vpop();
            if (op != TOK_SHL)
                vswap();
            if (c >= 32) {
                /* stack: L H */
                vpop();
                if (c > 32) {
                    vpushi(c - 32);
                    gen_op(op);
                }
                if (op != TOK_SAR) {
                    vpushi(0);
                } else {
                    gv_dup();
                    vpushi(31);
                    gen_op(TOK_SAR);
                }
                vswap();
            } else {
                vswap();
                gv_dup();
                /* stack: H L L */
                vpushi(c);
                gen_op(op);
                vswap();
                vpushi(32 - c);
                if (op == TOK_SHL)
                    gen_op(TOK_SHR);
                else
                    gen_op(TOK_SHL);
                vrotb(3);
                /* stack: L L H */
                vpushi(c);
                if (op == TOK_SHL)
                    gen_op(TOK_SHL);
                else
                    gen_op(TOK_SHR);
                gen_op('|');
            }
            if (op != TOK_SHL)
                vswap();
            lbuild(t);
        } else {
            /* XXX: should provide a faster fallback on x86 ? */
            switch(op) {
            case TOK_SAR:
                func = TOK___ashrdi3;
                goto gen_func;
            case TOK_SHR:
                func = TOK___lshrdi3;
                goto gen_func;
            case TOK_SHL:
                func = TOK___ashldi3;
                goto gen_func;
            }
        }
        break;
    default:
        /* compare operations */
        t = vtop->type.t;
        vswap();
        lexpand();
        vrotb(3);
        lexpand();
        /* stack: L1 H1 L2 H2 */
        tmp = vtop[-1];
        vtop[-1] = vtop[-2];
        vtop[-2] = tmp;
        /* stack: L1 L2 H1 H2 */
        /* compare high */
        op1 = op;
        /* when values are equal, we need to compare low words. since
           the jump is inverted, we invert the test too. */
        if (op1 == TOK_LT)
            op1 = TOK_LE;
        else if (op1 == TOK_GT)
            op1 = TOK_GE;
        else if (op1 == TOK_ULT)
            op1 = TOK_ULE;
        else if (op1 == TOK_UGT)
            op1 = TOK_UGE;
        a = 0;
        b = 0;
        gen_op(op1);
        if (op1 != TOK_NE) {
            a = gtst(1, 0);
        }
        if (op != TOK_EQ) {
            /* generate non equal test */
            /* XXX: NOT PORTABLE yet */
            if (a == 0) {
                b = gtst(0, 0);
            } else {
#if defined(TCC_TARGET_I386)
                b = psym(0x850f, 0);
#elif defined(TCC_TARGET_ARM)
                b = ind;
                o(0x1A000000 | encbranch(ind, 0, 1));
#elif defined(TCC_TARGET_C67)
                error("not implemented");
#else
#error not supported
#endif
            }
        }
        /* compare low. Always unsigned */
        op1 = op;
        if (op1 == TOK_LT)
            op1 = TOK_ULT;
        else if (op1 == TOK_LE)
            op1 = TOK_ULE;
        else if (op1 == TOK_GT)
            op1 = TOK_UGT;
        else if (op1 == TOK_GE)
            op1 = TOK_UGE;
        gen_op(op1);
        a = gtst(1, a);
        gsym(b);
        vseti(VT_JMPI, a);
        break;
    }
}
#endif

/* handle integer constant optimizations and various machine
   independent opt */
void gen_opic(int op)
{
    int c1, c2, t1, t2, n;
    SValue *v1, *v2;
    long long l1, l2;
    typedef unsigned long long U;

    v1 = vtop - 1;
    v2 = vtop;
    t1 = v1->type.t & VT_BTYPE;
    t2 = v2->type.t & VT_BTYPE;

    if (t1 == VT_LLONG)
        l1 = v1->c.ll;
    else if (v1->type.t & VT_UNSIGNED)
        l1 = v1->c.ui;
    else
        l1 = v1->c.i;

    if (t2 == VT_LLONG)
        l2 = v2->c.ll;
    else if (v2->type.t & VT_UNSIGNED)
        l2 = v2->c.ui;
    else
        l2 = v2->c.i;

    /* currently, we cannot do computations with forward symbols */
    c1 = (v1->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    c2 = (v2->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    if (c1 && c2) {
        switch(op) {
        case '+': l1 += l2; break;
        case '-': l1 -= l2; break;
        case '&': l1 &= l2; break;
        case '^': l1 ^= l2; break;
        case '|': l1 |= l2; break;
        case '*': l1 *= l2; break;

        case TOK_PDIV:
        case '/':
        case '%':
        case TOK_UDIV:
        case TOK_UMOD:
            /* if division by zero, generate explicit division */
            if (l2 == 0) {
                if (const_wanted)
                    error("division by zero in constant");
                goto general_case;
            }
            switch(op) {
            default: l1 /= l2; break;
            case '%': l1 %= l2; break;
            case TOK_UDIV: l1 = (U)l1 / l2; break;
            case TOK_UMOD: l1 = (U)l1 % l2; break;
            }
            break;
        case TOK_SHL: l1 <<= l2; break;
        case TOK_SHR: l1 = (U)l1 >> l2; break;
        case TOK_SAR: l1 >>= l2; break;
            /* tests */
        case TOK_ULT: l1 = (U)l1 < (U)l2; break;
        case TOK_UGE: l1 = (U)l1 >= (U)l2; break;
        case TOK_EQ: l1 = l1 == l2; break;
        case TOK_NE: l1 = l1 != l2; break;
        case TOK_ULE: l1 = (U)l1 <= (U)l2; break;
        case TOK_UGT: l1 = (U)l1 > (U)l2; break;
        case TOK_LT: l1 = l1 < l2; break;
        case TOK_GE: l1 = l1 >= l2; break;
        case TOK_LE: l1 = l1 <= l2; break;
        case TOK_GT: l1 = l1 > l2; break;
            /* logical */
        case TOK_LAND: l1 = l1 && l2; break;
        case TOK_LOR: l1 = l1 || l2; break;
        default:
            goto general_case;
        }
        v1->c.ll = l1;
        vtop--;
    } else {
        /* if commutative ops, put c2 as constant */
        if (c1 && (op == '+' || op == '&' || op == '^' || 
                   op == '|' || op == '*')) {
            vswap();
            c2 = c1; //c = c1, c1 = c2, c2 = c;
            l2 = l1; //l = l1, l1 = l2, l2 = l;
        }
        /* Filter out NOP operations like x*1, x-0, x&-1... */
        if (c2 && (((op == '*' || op == '/' || op == TOK_UDIV || 
                     op == TOK_PDIV) && 
                    l2 == 1) ||
                   ((op == '+' || op == '-' || op == '|' || op == '^' || 
                     op == TOK_SHL || op == TOK_SHR || op == TOK_SAR) && 
                    l2 == 0) ||
                   (op == '&' && 
                    l2 == -1))) {
            /* nothing to do */
            vtop--;
        } else if (c2 && (op == '*' || op == TOK_PDIV || op == TOK_UDIV)) {
            /* try to use shifts instead of muls or divs */
            if (l2 > 0 && (l2 & (l2 - 1)) == 0) {
                n = -1;
                while (l2) {
                    l2 >>= 1;
                    n++;
                }
                vtop->c.ll = n;
                if (op == '*')
                    op = TOK_SHL;
                else if (op == TOK_PDIV)
                    op = TOK_SAR;
                else
                    op = TOK_SHR;
            }
            goto general_case;
        } else if (c2 && (op == '+' || op == '-') &&
                   ((vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) ==
                   (VT_CONST | VT_SYM) ||
		   (vtop[-1].r & (VT_VALMASK | VT_LVAL)) == VT_LOCAL)) {
            /* symbol + constant case */
            if (op == '-')
                l2 = -l2;
            vtop--;
            vtop->c.ll += l2;
        } else {
        general_case:
            if (!nocode_wanted) {
                /* call low level op generator */
                if (t1 == VT_LLONG || t2 == VT_LLONG) 
                    gen_opl(op);
                else
                    gen_opi(op);
            } else {
                vtop--;
            }
        }
    }
}

/* generate a floating point operation with constant propagation */
void gen_opif(int op)
{
    int c1, c2;
    SValue *v1, *v2;
    long double f1, f2;

    v1 = vtop - 1;
    v2 = vtop;
    /* currently, we cannot do computations with forward symbols */
    c1 = (v1->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    c2 = (v2->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    if (c1 && c2) {
        if (v1->type.t == VT_FLOAT) {
            f1 = v1->c.f;
            f2 = v2->c.f;
        } else if (v1->type.t == VT_DOUBLE) {
            f1 = v1->c.d;
            f2 = v2->c.d;
        } else {
            f1 = v1->c.ld;
            f2 = v2->c.ld;
        }

        /* NOTE: we only do constant propagation if finite number (not
           NaN or infinity) (ANSI spec) */
        if (!ieee_finite(f1) || !ieee_finite(f2))
            goto general_case;

        switch(op) {
        case '+': f1 += f2; break;
        case '-': f1 -= f2; break;
        case '*': f1 *= f2; break;
        case '/': 
            if (f2 == 0.0) {
                if (const_wanted)
                    error("division by zero in constant");
                goto general_case;
            }
            f1 /= f2; 
            break;
            /* XXX: also handles tests ? */
        default:
            goto general_case;
        }
        /* XXX: overflow test ? */
        if (v1->type.t == VT_FLOAT) {
            v1->c.f = f1;
        } else if (v1->type.t == VT_DOUBLE) {
            v1->c.d = f1;
        } else {
            v1->c.ld = f1;
        }
        vtop--;
    } else {
    general_case:
        if (!nocode_wanted) {
            gen_opf(op);
        } else {
            vtop--;
        }
    }
}

static int pointed_size(CType *type)
{
    int align;
    return type_size(pointed_type(type), &align);
}

static inline int is_null_pointer(SValue *p)
{
    if ((p->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
        return 0;
    return ((p->type.t & VT_BTYPE) == VT_INT && p->c.i == 0) ||
        ((p->type.t & VT_BTYPE) == VT_LLONG && p->c.ll == 0);
}

static inline int is_integer_btype(int bt)
{
    return (bt == VT_BYTE || bt == VT_SHORT || 
            bt == VT_INT || bt == VT_LLONG);
}

/* check types for comparison or substraction of pointers */
static void check_comparison_pointer_types(SValue *p1, SValue *p2, int op)
{
    CType *type1, *type2, tmp_type1, tmp_type2;
    int bt1, bt2;
    
    /* null pointers are accepted for all comparisons as gcc */
    if (is_null_pointer(p1) || is_null_pointer(p2))
        return;
    type1 = &p1->type;
    type2 = &p2->type;
    bt1 = type1->t & VT_BTYPE;
    bt2 = type2->t & VT_BTYPE;
    /* accept comparison between pointer and integer with a warning */
    if ((is_integer_btype(bt1) || is_integer_btype(bt2)) && op != '-') {
        if (op != TOK_LOR && op != TOK_LAND )
            warning("comparison between pointer and integer");
        return;
    }

    /* both must be pointers or implicit function pointers */
    if (bt1 == VT_PTR) {
        type1 = pointed_type(type1);
    } else if (bt1 != VT_FUNC) 
        goto invalid_operands;

    if (bt2 == VT_PTR) {
        type2 = pointed_type(type2);
    } else if (bt2 != VT_FUNC) { 
    invalid_operands:
        error("invalid operands to binary %s", get_tok_str(op, NULL));
    }
    if ((type1->t & VT_BTYPE) == VT_VOID || 
        (type2->t & VT_BTYPE) == VT_VOID)
        return;
    tmp_type1 = *type1;
    tmp_type2 = *type2;
    tmp_type1.t &= ~(VT_UNSIGNED | VT_CONSTANT | VT_VOLATILE);
    tmp_type2.t &= ~(VT_UNSIGNED | VT_CONSTANT | VT_VOLATILE);
    if (!is_compatible_types(&tmp_type1, &tmp_type2)) {
        /* gcc-like error if '-' is used */
        if (op == '-')
            goto invalid_operands;
        else
            warning("comparison of distinct pointer types lacks a cast");
    }
}

/* generic gen_op: handles types problems */
void gen_op(int op)
{
    int u, t1, t2, bt1, bt2, t;
    CType type1;

    t1 = vtop[-1].type.t;
    t2 = vtop[0].type.t;
    bt1 = t1 & VT_BTYPE;
    bt2 = t2 & VT_BTYPE;
        
    if (bt1 == VT_PTR || bt2 == VT_PTR) {
        /* at least one operand is a pointer */
        /* relationnal op: must be both pointers */
        if (op >= TOK_ULT && op <= TOK_LOR) {
            check_comparison_pointer_types(vtop - 1, vtop, op);
            /* pointers are handled are unsigned */
#ifdef TCC_TARGET_X86_64
            t = VT_LLONG | VT_UNSIGNED;
#else
            t = VT_INT | VT_UNSIGNED;
#endif
            goto std_op;
        }
        /* if both pointers, then it must be the '-' op */
        if (bt1 == VT_PTR && bt2 == VT_PTR) {
            if (op != '-')
                error("cannot use pointers here");
            check_comparison_pointer_types(vtop - 1, vtop, op);
            /* XXX: check that types are compatible */
            u = pointed_size(&vtop[-1].type);
            gen_opic(op);
            /* set to integer type */
#ifdef TCC_TARGET_X86_64
            vtop->type.t = VT_LLONG;
#else
            vtop->type.t = VT_INT; 
#endif
            vpushi(u);
            gen_op(TOK_PDIV);
        } else {
            /* exactly one pointer : must be '+' or '-'. */
            if (op != '-' && op != '+')
                error("cannot use pointers here");
            /* Put pointer as first operand */
            if (bt2 == VT_PTR) {
                vswap();
                swap(&t1, &t2);
            }
            type1 = vtop[-1].type;
#ifdef TCC_TARGET_X86_64
            vpushll(pointed_size(&vtop[-1].type));
#else
            /* XXX: cast to int ? (long long case) */
            vpushi(pointed_size(&vtop[-1].type));
#endif
            gen_op('*');
#ifdef CONFIG_TCC_BCHECK
            /* if evaluating constant expression, no code should be
               generated, so no bound check */
            if (do_bounds_check && !const_wanted) {
                /* if bounded pointers, we generate a special code to
                   test bounds */
                if (op == '-') {
                    vpushi(0);
                    vswap();
                    gen_op('-');
                }
                gen_bounded_ptr_add();
            } else
#endif
            {
                gen_opic(op);
            }
            /* put again type if gen_opic() swaped operands */
            vtop->type = type1;
        }
    } else if (is_float(bt1) || is_float(bt2)) {
        /* compute bigger type and do implicit casts */
        if (bt1 == VT_LDOUBLE || bt2 == VT_LDOUBLE) {
            t = VT_LDOUBLE;
        } else if (bt1 == VT_DOUBLE || bt2 == VT_DOUBLE) {
            t = VT_DOUBLE;
        } else {
            t = VT_FLOAT;
        }
        /* floats can only be used for a few operations */
        if (op != '+' && op != '-' && op != '*' && op != '/' &&
            (op < TOK_ULT || op > TOK_GT))
            error("invalid operands for binary operation");
        goto std_op;
    } else if (bt1 == VT_LLONG || bt2 == VT_LLONG) {
        /* cast to biggest op */
        t = VT_LLONG;
        /* convert to unsigned if it does not fit in a long long */
        if ((t1 & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED) ||
            (t2 & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED))
            t |= VT_UNSIGNED;
        goto std_op;
    } else {
        /* integer operations */
        t = VT_INT;
        /* convert to unsigned if it does not fit in an integer */
        if ((t1 & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED) ||
            (t2 & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED))
            t |= VT_UNSIGNED;
    std_op:
        /* XXX: currently, some unsigned operations are explicit, so
           we modify them here */
        if (t & VT_UNSIGNED) {
            if (op == TOK_SAR)
                op = TOK_SHR;
            else if (op == '/')
                op = TOK_UDIV;
            else if (op == '%')
                op = TOK_UMOD;
            else if (op == TOK_LT)
                op = TOK_ULT;
            else if (op == TOK_GT)
                op = TOK_UGT;
            else if (op == TOK_LE)
                op = TOK_ULE;
            else if (op == TOK_GE)
                op = TOK_UGE;
        }
        vswap();
        type1.t = t;
        gen_cast(&type1);
        vswap();
        /* special case for shifts and long long: we keep the shift as
           an integer */
        if (op == TOK_SHR || op == TOK_SAR || op == TOK_SHL)
            type1.t = VT_INT;
        gen_cast(&type1);
        if (is_float(t))
            gen_opif(op);
        else
            gen_opic(op);
        if (op >= TOK_ULT && op <= TOK_GT) {
            /* relationnal op: the result is an int */
            vtop->type.t = VT_INT;
        } else {
            vtop->type.t = t;
        }
    }
}

#ifndef TCC_TARGET_ARM
/* generic itof for unsigned long long case */
void gen_cvt_itof1(int t)
{
    if ((vtop->type.t & (VT_BTYPE | VT_UNSIGNED)) == 
        (VT_LLONG | VT_UNSIGNED)) {

        if (t == VT_FLOAT)
            vpush_global_sym(&func_old_type, TOK___floatundisf);
#if LDOUBLE_SIZE != 8
        else if (t == VT_LDOUBLE)
            vpush_global_sym(&func_old_type, TOK___floatundixf);
#endif
        else
            vpush_global_sym(&func_old_type, TOK___floatundidf);
        vrott(2);
        gfunc_call(1);
        vpushi(0);
        vtop->r = reg_fret(t);
    } else {
        gen_cvt_itof(t);
    }
}
#endif

/* generic ftoi for unsigned long long case */
void gen_cvt_ftoi1(int t)
{
    int st;

    if (t == (VT_LLONG | VT_UNSIGNED)) {
        /* not handled natively */
        st = vtop->type.t & VT_BTYPE;
        if (st == VT_FLOAT)
            vpush_global_sym(&func_old_type, TOK___fixunssfdi);
#if LDOUBLE_SIZE != 8
        else if (st == VT_LDOUBLE)
            vpush_global_sym(&func_old_type, TOK___fixunsxfdi);
#endif
        else
            vpush_global_sym(&func_old_type, TOK___fixunsdfdi);
        vrott(2);
        gfunc_call(1);
        vpushi(0);
        vtop->r = REG_IRET;
        vtop->r2 = REG_LRET;
    } else {
        gen_cvt_ftoi(t);
    }
}

/* force char or short cast */
void force_charshort_cast(int t)
{
    int bits, dbt;
    dbt = t & VT_BTYPE;
    /* XXX: add optimization if lvalue : just change type and offset */
    if (dbt == VT_BYTE)
        bits = 8;
    else
        bits = 16;
    if (t & VT_UNSIGNED) {
        vpushi((1 << bits) - 1);
        gen_op('&');
    } else {
        bits = 32 - bits;
        vpushi(bits);
        gen_op(TOK_SHL);
        /* result must be signed or the SAR is converted to an SHL
           This was not the case when "t" was a signed short
           and the last value on the stack was an unsigned int */
        vtop->type.t &= ~VT_UNSIGNED;
        vpushi(bits);
        gen_op(TOK_SAR);
    }
}

/* cast 'vtop' to 'type'. Casting to bitfields is forbidden. */
static void gen_cast(CType *type)
{
    int sbt, dbt, sf, df, c, p;

    /* special delayed cast for char/short */
    /* XXX: in some cases (multiple cascaded casts), it may still
       be incorrect */
    if (vtop->r & VT_MUSTCAST) {
        vtop->r &= ~VT_MUSTCAST;
        force_charshort_cast(vtop->type.t);
    }

    /* bitfields first get cast to ints */
    if (vtop->type.t & VT_BITFIELD) {
        gv(RC_INT);
    }

    dbt = type->t & (VT_BTYPE | VT_UNSIGNED);
    sbt = vtop->type.t & (VT_BTYPE | VT_UNSIGNED);

    if (sbt != dbt) {
        sf = is_float(sbt);
        df = is_float(dbt);
        c = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
        p = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_CONST | VT_SYM);
        if (c) {
            /* constant case: we can do it now */
            /* XXX: in ISOC, cannot do it if error in convert */
            if (sbt == VT_FLOAT)
                vtop->c.ld = vtop->c.f;
            else if (sbt == VT_DOUBLE)
                vtop->c.ld = vtop->c.d;

            if (df) {
                if ((sbt & VT_BTYPE) == VT_LLONG) {
                    if (sbt & VT_UNSIGNED)
                        vtop->c.ld = vtop->c.ull;
                    else
                        vtop->c.ld = vtop->c.ll;
                } else if(!sf) {
                    if (sbt & VT_UNSIGNED)
                        vtop->c.ld = vtop->c.ui;
                    else
                        vtop->c.ld = vtop->c.i;
                }

                if (dbt == VT_FLOAT)
                    vtop->c.f = (float)vtop->c.ld;
                else if (dbt == VT_DOUBLE)
                    vtop->c.d = (double)vtop->c.ld;
            } else if (sf && dbt == (VT_LLONG|VT_UNSIGNED)) {
                vtop->c.ull = (unsigned long long)vtop->c.ld;
            } else if (sf && dbt == VT_BOOL) {
                vtop->c.i = (vtop->c.ld != 0);
            } else {
                if(sf)
                    vtop->c.ll = (long long)vtop->c.ld;
                else if (sbt == (VT_LLONG|VT_UNSIGNED))
                    vtop->c.ll = vtop->c.ull;
                else if (sbt & VT_UNSIGNED)
                    vtop->c.ll = vtop->c.ui;
                else if (sbt != VT_LLONG)
                    vtop->c.ll = vtop->c.i;

                if (dbt == (VT_LLONG|VT_UNSIGNED))
                    vtop->c.ull = vtop->c.ll;
                else if (dbt == VT_BOOL)
                    vtop->c.i = (vtop->c.ll != 0);
                else if (dbt != VT_LLONG) {
                    int s = 0;
                    if ((dbt & VT_BTYPE) == VT_BYTE)
                        s = 24;
                    else if ((dbt & VT_BTYPE) == VT_SHORT)
                        s = 16;

                    if(dbt & VT_UNSIGNED)
                        vtop->c.ui = ((unsigned int)vtop->c.ll << s) >> s;
                    else
                        vtop->c.i = ((int)vtop->c.ll << s) >> s;
                }
            }
        } else if (p && dbt == VT_BOOL) {
            vtop->r = VT_CONST;
            vtop->c.i = 1;
        } else if (!nocode_wanted) {
            /* non constant case: generate code */
            if (sf && df) {
                /* convert from fp to fp */
                gen_cvt_ftof(dbt);
            } else if (df) {
                /* convert int to fp */
                gen_cvt_itof1(dbt);
            } else if (sf) {
                /* convert fp to int */
                if (dbt == VT_BOOL) {
                     vpushi(0);
                     gen_op(TOK_NE);
                } else {
                    /* we handle char/short/etc... with generic code */
                    if (dbt != (VT_INT | VT_UNSIGNED) &&
                        dbt != (VT_LLONG | VT_UNSIGNED) &&
                        dbt != VT_LLONG)
                        dbt = VT_INT;
                    gen_cvt_ftoi1(dbt);
                    if (dbt == VT_INT && (type->t & (VT_BTYPE | VT_UNSIGNED)) != dbt) {
                        /* additional cast for char/short... */
                        vtop->type.t = dbt;
                        gen_cast(type);
                    }
                }
#ifndef TCC_TARGET_X86_64
            } else if ((dbt & VT_BTYPE) == VT_LLONG) {
                if ((sbt & VT_BTYPE) != VT_LLONG) {
                    /* scalar to long long */
                    /* machine independent conversion */
                    gv(RC_INT);
                    /* generate high word */
                    if (sbt == (VT_INT | VT_UNSIGNED)) {
                        vpushi(0);
                        gv(RC_INT);
                    } else {
                        if (sbt == VT_PTR) {
                            /* cast from pointer to int before we apply
                               shift operation, which pointers don't support*/
                            gen_cast(&int_type);
                        }
                        gv_dup();
                        vpushi(31);
                        gen_op(TOK_SAR);
                    }
                    /* patch second register */
                    vtop[-1].r2 = vtop->r;
                    vpop();
                }
#else
            } else if ((dbt & VT_BTYPE) == VT_LLONG ||
                       (dbt & VT_BTYPE) == VT_PTR) {
                /* XXX: not sure if this is perfect... need more tests */
                if ((sbt & VT_BTYPE) != VT_LLONG) {
                    int r = gv(RC_INT);
                    if (sbt != (VT_INT | VT_UNSIGNED) &&
                        sbt != VT_PTR && sbt != VT_FUNC) {
                        /* x86_64 specific: movslq */
                        o(0x6348);
                        o(0xc0 + (REG_VALUE(r) << 3) + REG_VALUE(r));
                    }
                }
#endif
            } else if (dbt == VT_BOOL) {
                /* scalar to bool */
                vpushi(0);
                gen_op(TOK_NE);
            } else if ((dbt & VT_BTYPE) == VT_BYTE || 
                       (dbt & VT_BTYPE) == VT_SHORT) {
                if (sbt == VT_PTR) {
                    vtop->type.t = VT_INT;
                    warning("nonportable conversion from pointer to char/short");
                }
                force_charshort_cast(dbt);
            } else if ((dbt & VT_BTYPE) == VT_INT) {
                /* scalar to int */
                if (sbt == VT_LLONG) {
                    /* from long long: just take low order word */
                    lexpand();
                    vpop();
                } 
                /* if lvalue and single word type, nothing to do because
                   the lvalue already contains the real type size (see
                   VT_LVAL_xxx constants) */
            }
        }
    } else if ((dbt & VT_BTYPE) == VT_PTR && !(vtop->r & VT_LVAL)) {
        /* if we are casting between pointer types,
           we must update the VT_LVAL_xxx size */
        vtop->r = (vtop->r & ~VT_LVAL_TYPE)
                  | (lvalue_type(type->ref->type.t) & VT_LVAL_TYPE);
    }
    vtop->type = *type;
}

/* return type size. Put alignment at 'a' */
int type_size(CType *type, int *a)
{
    Sym *s;
    int bt;

    bt = type->t & VT_BTYPE;
    if (bt == VT_STRUCT) {
        /* struct/union */
        s = type->ref;
        *a = s->r;
        return s->c;
    } else if (bt == VT_PTR) {
        if (type->t & VT_ARRAY) {
            int ts;

            s = type->ref;
            ts = type_size(&s->type, a);

            if (ts < 0 && s->c < 0)
                ts = -ts;

            return ts * s->c;
        } else {
            *a = PTR_SIZE;
            return PTR_SIZE;
        }
    } else if (bt == VT_LDOUBLE) {
        *a = LDOUBLE_ALIGN;
        return LDOUBLE_SIZE;
    } else if (bt == VT_DOUBLE || bt == VT_LLONG) {
#ifdef TCC_TARGET_I386
#ifdef TCC_TARGET_PE
        *a = 8;
#else
        *a = 4;
#endif
#elif defined(TCC_TARGET_ARM)
#ifdef TCC_ARM_EABI
        *a = 8; 
#else
        *a = 4;
#endif
#else
        *a = 8;
#endif
        return 8;
    } else if (bt == VT_INT || bt == VT_ENUM || bt == VT_FLOAT) {
        *a = 4;
        return 4;
    } else if (bt == VT_SHORT) {
        *a = 2;
        return 2;
    } else {
        /* char, void, function, _Bool */
        *a = 1;
        return 1;
    }
}

/* return the pointed type of t */
static inline CType *pointed_type(CType *type)
{
    return &type->ref->type;
}

/* modify type so that its it is a pointer to type. */
static void mk_pointer(CType *type)
{
    Sym *s;
    s = sym_push(SYM_FIELD, type, 0, -1);
    type->t = VT_PTR | (type->t & ~VT_TYPE);
    type->ref = s;
}

/* compare function types. OLD functions match any new functions */
static int is_compatible_func(CType *type1, CType *type2)
{
    Sym *s1, *s2;

    s1 = type1->ref;
    s2 = type2->ref;
    if (!is_compatible_types(&s1->type, &s2->type))
        return 0;
    /* check func_call */
    if (FUNC_CALL(s1->r) != FUNC_CALL(s2->r))
        return 0;
    /* XXX: not complete */
    if (s1->c == FUNC_OLD || s2->c == FUNC_OLD)
        return 1;
    if (s1->c != s2->c)
        return 0;
    while (s1 != NULL) {
        if (s2 == NULL)
            return 0;
        if (!is_compatible_parameter_types(&s1->type, &s2->type))
            return 0;
        s1 = s1->next;
        s2 = s2->next;
    }
    if (s2)
        return 0;
    return 1;
}

/* return true if type1 and type2 are the same.  If unqualified is
   true, qualifiers on the types are ignored.

   - enums are not checked as gcc __builtin_types_compatible_p () 
 */
static int compare_types(CType *type1, CType *type2, int unqualified)
{
    int bt1, t1, t2;

    t1 = type1->t & VT_TYPE;
    t2 = type2->t & VT_TYPE;
    if (unqualified) {
        /* strip qualifiers before comparing */
        t1 &= ~(VT_CONSTANT | VT_VOLATILE);
        t2 &= ~(VT_CONSTANT | VT_VOLATILE);
    }
    /* XXX: bitfields ? */
    if (t1 != t2)
        return 0;
    /* test more complicated cases */
    bt1 = t1 & VT_BTYPE;
    if (bt1 == VT_PTR) {
        type1 = pointed_type(type1);
        type2 = pointed_type(type2);
        return is_compatible_types(type1, type2);
    } else if (bt1 == VT_STRUCT) {
        return (type1->ref == type2->ref);
    } else if (bt1 == VT_FUNC) {
        return is_compatible_func(type1, type2);
    } else {
        return 1;
    }
}

/* return true if type1 and type2 are exactly the same (including
   qualifiers). 
*/
static int is_compatible_types(CType *type1, CType *type2)
{
    return compare_types(type1,type2,0);
}

/* return true if type1 and type2 are the same (ignoring qualifiers).
*/
static int is_compatible_parameter_types(CType *type1, CType *type2)
{
    return compare_types(type1,type2,1);
}

/* print a type. If 'varstr' is not NULL, then the variable is also
   printed in the type */
/* XXX: union */
/* XXX: add array and function pointers */
void type_to_str(char *buf, int buf_size, 
                 CType *type, const char *varstr)
{
    int bt, v, t;
    Sym *s, *sa;
    char buf1[256];
    const char *tstr;

    t = type->t & VT_TYPE;
    bt = t & VT_BTYPE;
    buf[0] = '\0';
    if (t & VT_CONSTANT)
        pstrcat(buf, buf_size, "const ");
    if (t & VT_VOLATILE)
        pstrcat(buf, buf_size, "volatile ");
    if (t & VT_UNSIGNED)
        pstrcat(buf, buf_size, "unsigned ");
    switch(bt) {
    case VT_VOID:
        tstr = "void";
        goto add_tstr;
    case VT_BOOL:
        tstr = "_Bool";
        goto add_tstr;
    case VT_BYTE:
        tstr = "char";
        goto add_tstr;
    case VT_SHORT:
        tstr = "short";
        goto add_tstr;
    case VT_INT:
        tstr = "int";
        goto add_tstr;
    case VT_LONG:
        tstr = "long";
        goto add_tstr;
    case VT_LLONG:
        tstr = "long long";
        goto add_tstr;
    case VT_FLOAT:
        tstr = "float";
        goto add_tstr;
    case VT_DOUBLE:
        tstr = "double";
        goto add_tstr;
    case VT_LDOUBLE:
        tstr = "long double";
    add_tstr:
        pstrcat(buf, buf_size, tstr);
        break;
    case VT_ENUM:
    case VT_STRUCT:
        if (bt == VT_STRUCT)
            tstr = "struct ";
        else
            tstr = "enum ";
        pstrcat(buf, buf_size, tstr);
        v = type->ref->v & ~SYM_STRUCT;
        if (v >= SYM_FIRST_ANOM)
            pstrcat(buf, buf_size, "<anonymous>");
        else
            pstrcat(buf, buf_size, get_tok_str(v, NULL));
        break;
    case VT_FUNC:
        s = type->ref;
        type_to_str(buf, buf_size, &s->type, varstr);
        pstrcat(buf, buf_size, "(");
        sa = s->next;
        while (sa != NULL) {
            type_to_str(buf1, sizeof(buf1), &sa->type, NULL);
            pstrcat(buf, buf_size, buf1);
            sa = sa->next;
            if (sa)
                pstrcat(buf, buf_size, ", ");
        }
        pstrcat(buf, buf_size, ")");
        goto no_var;
    case VT_PTR:
        s = type->ref;
        pstrcpy(buf1, sizeof(buf1), "*");
        if (varstr)
            pstrcat(buf1, sizeof(buf1), varstr);
        type_to_str(buf, buf_size, &s->type, buf1);
        goto no_var;
    }
    if (varstr) {
        pstrcat(buf, buf_size, " ");
        pstrcat(buf, buf_size, varstr);
    }
 no_var: ;
}

/* verify type compatibility to store vtop in 'dt' type, and generate
   casts if needed. */
static void gen_assign_cast(CType *dt)
{
    CType *st, *type1, *type2, tmp_type1, tmp_type2;
    char buf1[256], buf2[256];
    int dbt, sbt;

    st = &vtop->type; /* source type */
    dbt = dt->t & VT_BTYPE;
    sbt = st->t & VT_BTYPE;
    if (dt->t & VT_CONSTANT)
        warning("assignment of read-only location");
    switch(dbt) {
    case VT_PTR:
        /* special cases for pointers */
        /* '0' can also be a pointer */
        if (is_null_pointer(vtop))
            goto type_ok;
        /* accept implicit pointer to integer cast with warning */
        if (is_integer_btype(sbt)) {
            warning("assignment makes pointer from integer without a cast");
            goto type_ok;
        }
        type1 = pointed_type(dt);
        /* a function is implicitely a function pointer */
        if (sbt == VT_FUNC) {
            if ((type1->t & VT_BTYPE) != VT_VOID &&
                !is_compatible_types(pointed_type(dt), st))
                goto error;
            else
                goto type_ok;
        }
        if (sbt != VT_PTR)
            goto error;
        type2 = pointed_type(st);
        if ((type1->t & VT_BTYPE) == VT_VOID || 
            (type2->t & VT_BTYPE) == VT_VOID) {
            /* void * can match anything */
        } else {
            /* exact type match, except for unsigned */
            tmp_type1 = *type1;
            tmp_type2 = *type2;
            tmp_type1.t &= ~(VT_UNSIGNED | VT_CONSTANT | VT_VOLATILE);
            tmp_type2.t &= ~(VT_UNSIGNED | VT_CONSTANT | VT_VOLATILE);
            if (!is_compatible_types(&tmp_type1, &tmp_type2))
                warning("assignment from incompatible pointer type");
        }
        /* check const and volatile */
        if ((!(type1->t & VT_CONSTANT) && (type2->t & VT_CONSTANT)) ||
            (!(type1->t & VT_VOLATILE) && (type2->t & VT_VOLATILE)))
            warning("assignment discards qualifiers from pointer target type");
        break;
    case VT_BYTE:
    case VT_SHORT:
    case VT_INT:
    case VT_LLONG:
        if (sbt == VT_PTR || sbt == VT_FUNC) {
            warning("assignment makes integer from pointer without a cast");
        }
        /* XXX: more tests */
        break;
    case VT_STRUCT:
        tmp_type1 = *dt;
        tmp_type2 = *st;
        tmp_type1.t &= ~(VT_CONSTANT | VT_VOLATILE);
        tmp_type2.t &= ~(VT_CONSTANT | VT_VOLATILE);
        if (!is_compatible_types(&tmp_type1, &tmp_type2)) {
        error:
            type_to_str(buf1, sizeof(buf1), st, NULL);
            type_to_str(buf2, sizeof(buf2), dt, NULL);
            error("cannot cast '%s' to '%s'", buf1, buf2);
        }
        break;
    }
 type_ok:
    gen_cast(dt);
}

/* store vtop in lvalue pushed on stack */
void vstore(void)
{
    int sbt, dbt, ft, r, t, size, align, bit_size, bit_pos, rc, delayed_cast;

    ft = vtop[-1].type.t;
    sbt = vtop->type.t & VT_BTYPE;
    dbt = ft & VT_BTYPE;
    if (((sbt == VT_INT || sbt == VT_SHORT) && dbt == VT_BYTE) ||
        (sbt == VT_INT && dbt == VT_SHORT)) {
        /* optimize char/short casts */
        delayed_cast = VT_MUSTCAST;
        vtop->type.t = ft & VT_TYPE;
        /* XXX: factorize */
        if (ft & VT_CONSTANT)
            warning("assignment of read-only location");
    } else {
        delayed_cast = 0;
        if (!(ft & VT_BITFIELD))
            gen_assign_cast(&vtop[-1].type);
    }

    if (sbt == VT_STRUCT) {
        /* if structure, only generate pointer */
        /* structure assignment : generate memcpy */
        /* XXX: optimize if small size */
        if (!nocode_wanted) {
            size = type_size(&vtop->type, &align);

#ifdef TCC_ARM_EABI
            if(!(align & 7))
                vpush_global_sym(&func_old_type, TOK_memcpy8);
            else if(!(align & 3))
                vpush_global_sym(&func_old_type, TOK_memcpy4);
            else
#endif
            vpush_global_sym(&func_old_type, TOK_memcpy);

            /* destination */
            vpushv(vtop - 2);
            vtop->type.t = VT_PTR;
            gaddrof();
            /* source */
            vpushv(vtop - 2);
            vtop->type.t = VT_PTR;
            gaddrof();
            /* type size */
            vpushi(size);
            gfunc_call(3);
            
            vswap();
            vpop();
        } else {
            vswap();
            vpop();
        }
        /* leave source on stack */
    } else if (ft & VT_BITFIELD) {
        /* bitfield store handling */
        bit_pos = (ft >> VT_STRUCT_SHIFT) & 0x3f;
        bit_size = (ft >> (VT_STRUCT_SHIFT + 6)) & 0x3f;
        /* remove bit field info to avoid loops */
        vtop[-1].type.t = ft & ~(VT_BITFIELD | (-1 << VT_STRUCT_SHIFT));

        /* duplicate source into other register */
        gv_dup();
        vswap();
        vrott(3);

        /* duplicate destination */
        vdup();
        vtop[-1] = vtop[-2];

        /* mask and shift source */
        if((ft & VT_BTYPE) != VT_BOOL) {
            if((ft & VT_BTYPE) == VT_LLONG) {
                vpushll((1ULL << bit_size) - 1ULL);
            } else {
                vpushi((1 << bit_size) - 1);
            }
            gen_op('&');
        }
        vpushi(bit_pos);
        gen_op(TOK_SHL);
        /* load destination, mask and or with source */
        vswap();
        if((ft & VT_BTYPE) == VT_LLONG) {
            vpushll(~(((1ULL << bit_size) - 1ULL) << bit_pos));
        } else {
            vpushi(~(((1 << bit_size) - 1) << bit_pos));
        }
        gen_op('&');
        gen_op('|');
        /* store result */
        vstore();

        /* pop off shifted source from "duplicate source..." above */
        vpop();

    } else {
#ifdef CONFIG_TCC_BCHECK
        /* bound check case */
        if (vtop[-1].r & VT_MUSTBOUND) {
            vswap();
            gbound();
            vswap();
        }
#endif
        if (!nocode_wanted) {
            rc = RC_INT;
            if (is_float(ft)) {
                rc = RC_FLOAT;
#ifdef TCC_TARGET_X86_64
                if ((ft & VT_BTYPE) == VT_LDOUBLE) {
                    rc = RC_ST0;
                }
#endif
            }
            r = gv(rc);  /* generate value */
            /* if lvalue was saved on stack, must read it */
            if ((vtop[-1].r & VT_VALMASK) == VT_LLOCAL) {
                SValue sv;
                t = get_reg(RC_INT);
#ifdef TCC_TARGET_X86_64
                sv.type.t = VT_PTR;
#else
                sv.type.t = VT_INT;
#endif
                sv.r = VT_LOCAL | VT_LVAL;
                sv.c.ul = vtop[-1].c.ul;
                load(t, &sv);
                vtop[-1].r = t | VT_LVAL;
            }
            store(r, vtop - 1);
#ifndef TCC_TARGET_X86_64
            /* two word case handling : store second register at word + 4 */
            if ((ft & VT_BTYPE) == VT_LLONG) {
                vswap();
                /* convert to int to increment easily */
                vtop->type.t = VT_INT;
                gaddrof();
                vpushi(4);
                gen_op('+');
                vtop->r |= VT_LVAL;
                vswap();
                /* XXX: it works because r2 is spilled last ! */
                store(vtop->r2, vtop - 1);
            }
#endif
        }
        vswap();
        vtop--; /* NOT vpop() because on x86 it would flush the fp stack */
        vtop->r |= delayed_cast;
    }
}

/* post defines POST/PRE add. c is the token ++ or -- */
void inc(int post, int c)
{
    test_lvalue();
    vdup(); /* save lvalue */
    if (post) {
        gv_dup(); /* duplicate value */
        vrotb(3);
        vrotb(3);
    }
    /* add constant */
    vpushi(c - TOK_MID); 
    gen_op('+');
    vstore(); /* store value */
    if (post)
        vpop(); /* if post op, return saved value */
}

/* Parse GNUC __attribute__ extension. Currently, the following
   extensions are recognized:
   - aligned(n) : set data/function alignment.
   - packed : force data alignment to 1
   - section(x) : generate data/code in this section.
   - unused : currently ignored, but may be used someday.
   - regparm(n) : pass function parameters in registers (i386 only)
 */
static void parse_attribute(AttributeDef *ad)
{
    int t, n;
    
    while (tok == TOK_ATTRIBUTE1 || tok == TOK_ATTRIBUTE2) {
    next();
    skip('(');
    skip('(');
    while (tok != ')') {
        if (tok < TOK_IDENT)
            expect("attribute name");
        t = tok;
        next();
        switch(t) {
        case TOK_SECTION1:
        case TOK_SECTION2:
            skip('(');
            if (tok != TOK_STR)
                expect("section name");
            ad->section = find_section(tcc_state, (char *)tokc.cstr->data);
            next();
            skip(')');
            break;
        case TOK_ALIGNED1:
        case TOK_ALIGNED2:
            if (tok == '(') {
                next();
                n = expr_const();
                if (n <= 0 || (n & (n - 1)) != 0) 
                    error("alignment must be a positive power of two");
                skip(')');
            } else {
                n = MAX_ALIGN;
            }
            ad->aligned = n;
            break;
        case TOK_PACKED1:
        case TOK_PACKED2:
            ad->packed = 1;
            break;
        case TOK_UNUSED1:
        case TOK_UNUSED2:
            /* currently, no need to handle it because tcc does not
               track unused objects */
            break;
        case TOK_NORETURN1:
        case TOK_NORETURN2:
            /* currently, no need to handle it because tcc does not
               track unused objects */
            break;
        case TOK_CDECL1:
        case TOK_CDECL2:
        case TOK_CDECL3:
            FUNC_CALL(ad->func_attr) = FUNC_CDECL;
            break;
        case TOK_STDCALL1:
        case TOK_STDCALL2:
        case TOK_STDCALL3:
            FUNC_CALL(ad->func_attr) = FUNC_STDCALL;
            break;
#ifdef TCC_TARGET_I386
        case TOK_REGPARM1:
        case TOK_REGPARM2:
            skip('(');
            n = expr_const();
            if (n > 3) 
                n = 3;
            else if (n < 0)
                n = 0;
            if (n > 0)
                FUNC_CALL(ad->func_attr) = FUNC_FASTCALL1 + n - 1;
            skip(')');
            break;
        case TOK_FASTCALL1:
        case TOK_FASTCALL2:
        case TOK_FASTCALL3:
            FUNC_CALL(ad->func_attr) = FUNC_FASTCALLW;
            break;            
#endif
        case TOK_DLLEXPORT:
            FUNC_EXPORT(ad->func_attr) = 1;
            break;
        default:
            if (tcc_state->warn_unsupported)
                warning("'%s' attribute ignored", get_tok_str(t, NULL));
            /* skip parameters */
            if (tok == '(') {
                int parenthesis = 0;
                do {
                    if (tok == '(') 
                        parenthesis++;
                    else if (tok == ')') 
                        parenthesis--;
                    next();
                } while (parenthesis && tok != -1);
            }
            break;
        }
        if (tok != ',')
            break;
        next();
    }
    skip(')');
    skip(')');
    }
}

/* enum/struct/union declaration. u is either VT_ENUM or VT_STRUCT */
static void struct_decl(CType *type, int u)
{
    int a, v, size, align, maxalign, c, offset;
    int bit_size, bit_pos, bsize, bt, lbit_pos;
    Sym *s, *ss, *ass, **ps;
    AttributeDef ad;
    CType type1, btype;

    a = tok; /* save decl type */
    next();
    if (tok != '{') {
        v = tok;
        next();
        /* struct already defined ? return it */
        if (v < TOK_IDENT)
            expect("struct/union/enum name");
        s = struct_find(v);
        if (s) {
            if (s->type.t != a)
                error("invalid type");
            goto do_decl;
        }
    } else {
        v = anon_sym++;
    }
    type1.t = a;
    /* we put an undefined size for struct/union */
    s = sym_push(v | SYM_STRUCT, &type1, 0, -1);
    s->r = 0; /* default alignment is zero as gcc */
    /* put struct/union/enum name in type */
 do_decl:
    type->t = u;
    type->ref = s;
    
    if (tok == '{') {
        next();
        if (s->c != -1)
            error("struct/union/enum already defined");
        /* cannot be empty */
        c = 0;
        /* non empty enums are not allowed */
        if (a == TOK_ENUM) {
            for(;;) {
                v = tok;
                if (v < TOK_UIDENT)
                    expect("identifier");
                next();
                if (tok == '=') {
                    next();
                    c = expr_const();
                }
                /* enum symbols have static storage */
                ss = sym_push(v, &int_type, VT_CONST, c);
                ss->type.t |= VT_STATIC;
                if (tok != ',')
                    break;
                next();
                c++;
                /* NOTE: we accept a trailing comma */
                if (tok == '}')
                    break;
            }
            skip('}');
        } else {
            maxalign = 1;
            ps = &s->next;
            bit_pos = 0;
            offset = 0;
            while (tok != '}') {
                parse_btype(&btype, &ad);
                while (1) {
                    bit_size = -1;
                    v = 0;
                    type1 = btype;
                    if (tok != ':') {
                        type_decl(&type1, &ad, &v, TYPE_DIRECT | TYPE_ABSTRACT);
                        if (v == 0 && (type1.t & VT_BTYPE) != VT_STRUCT)
                            expect("identifier");
                        if ((type1.t & VT_BTYPE) == VT_FUNC ||
                            (type1.t & (VT_TYPEDEF | VT_STATIC | VT_EXTERN | VT_INLINE)))
                            error("invalid type for '%s'", 
                                  get_tok_str(v, NULL));
                    }
                    if (tok == ':') {
                        next();
                        bit_size = expr_const();
                        /* XXX: handle v = 0 case for messages */
                        if (bit_size < 0)
                            error("negative width in bit-field '%s'", 
                                  get_tok_str(v, NULL));
                        if (v && bit_size == 0)
                            error("zero width for bit-field '%s'", 
                                  get_tok_str(v, NULL));
                    }
                    size = type_size(&type1, &align);
                    if (ad.aligned) {
                        if (align < ad.aligned)
                            align = ad.aligned;
                    } else if (ad.packed) {
                        align = 1;
                    } else if (*tcc_state->pack_stack_ptr) {
                        if (align > *tcc_state->pack_stack_ptr)
                            align = *tcc_state->pack_stack_ptr;
                    }
                    lbit_pos = 0;
                    if (bit_size >= 0) {
                        bt = type1.t & VT_BTYPE;
                        if (bt != VT_INT && 
                            bt != VT_BYTE && 
                            bt != VT_SHORT &&
                            bt != VT_BOOL &&
                            bt != VT_ENUM &&
                            bt != VT_LLONG)
                            error("bitfields must have scalar type");
                        bsize = size * 8;
                        if (bit_size > bsize) {
                            error("width of '%s' exceeds its type",
                                  get_tok_str(v, NULL));
                        } else if (bit_size == bsize) {
                            /* no need for bit fields */
                            bit_pos = 0;
                        } else if (bit_size == 0) {
                            /* XXX: what to do if only padding in a
                               structure ? */
                            /* zero size: means to pad */
                            if (bit_pos > 0)
                                bit_pos = bsize;
                        } else {
                            /* we do not have enough room ? */
                            if ((bit_pos + bit_size) > bsize)
                                bit_pos = 0;
                            lbit_pos = bit_pos;
                            /* XXX: handle LSB first */
                            type1.t |= VT_BITFIELD | 
                                (bit_pos << VT_STRUCT_SHIFT) |
                                (bit_size << (VT_STRUCT_SHIFT + 6));
                            bit_pos += bit_size;
                        }
                    } else {
                        bit_pos = 0;
                    }
                    if (v != 0 || (type1.t & VT_BTYPE) == VT_STRUCT) {
                        /* add new memory data only if starting
                           bit field */
                        if (lbit_pos == 0) {
                            if (a == TOK_STRUCT) {
                                c = (c + align - 1) & -align;
                                offset = c;
                                if (size > 0)
                                    c += size;
                            } else {
                                offset = 0;
                                if (size > c)
                                    c = size;
                            }
                            if (align > maxalign)
                                maxalign = align;
                        }
#if 0
                        printf("add field %s offset=%d", 
                               get_tok_str(v, NULL), offset);
                        if (type1.t & VT_BITFIELD) {
                            printf(" pos=%d size=%d", 
                                   (type1.t >> VT_STRUCT_SHIFT) & 0x3f,
                                   (type1.t >> (VT_STRUCT_SHIFT + 6)) & 0x3f);
                        }
                        printf("\n");
#endif
                    }
                    if (v == 0 && (type1.t & VT_BTYPE) == VT_STRUCT) {
                        ass = type1.ref;
                        while ((ass = ass->next) != NULL) {
                           ss = sym_push(ass->v, &ass->type, 0, offset + ass->c);
                           *ps = ss;
                           ps = &ss->next;
                        }
                    } else if (v) {
                        ss = sym_push(v | SYM_FIELD, &type1, 0, offset);
                        *ps = ss;
                        ps = &ss->next;
                    }
                    if (tok == ';' || tok == TOK_EOF)
                        break;
                    skip(',');
                }
                skip(';');
            }
            skip('}');
            /* store size and alignment */
            s->c = (c + maxalign - 1) & -maxalign; 
            s->r = maxalign;
        }
    }
}

/* return 0 if no type declaration. otherwise, return the basic type
   and skip it. 
 */
static int parse_btype(CType *type, AttributeDef *ad)
{
    int t, u, type_found, typespec_found, typedef_found;
    Sym *s;
    CType type1;

    memset(ad, 0, sizeof(AttributeDef));
    type_found = 0;
    typespec_found = 0;
    typedef_found = 0;
    t = 0;
    while(1) {
        switch(tok) {
        case TOK_EXTENSION:
            /* currently, we really ignore extension */
            next();
            continue;

            /* basic types */
        case TOK_CHAR:
            u = VT_BYTE;
        basic_type:
            next();
        basic_type1:
            if ((t & VT_BTYPE) != 0)
                error("too many basic types");
            t |= u;
            typespec_found = 1;
            break;
        case TOK_VOID:
            u = VT_VOID;
            goto basic_type;
        case TOK_SHORT:
            u = VT_SHORT;
            goto basic_type;
        case TOK_INT:
            next();
            typespec_found = 1;
            break;
        case TOK_LONG:
            next();
            if ((t & VT_BTYPE) == VT_DOUBLE) {
                t = (t & ~VT_BTYPE) | VT_LDOUBLE;
            } else if ((t & VT_BTYPE) == VT_LONG) {
                t = (t & ~VT_BTYPE) | VT_LLONG;
            } else {
                u = VT_LONG;
                goto basic_type1;
            }
            break;
        case TOK_BOOL:
            u = VT_BOOL;
            goto basic_type;
        case TOK_FLOAT:
            u = VT_FLOAT;
            goto basic_type;
        case TOK_DOUBLE:
            next();
            if ((t & VT_BTYPE) == VT_LONG) {
                t = (t & ~VT_BTYPE) | VT_LDOUBLE;
            } else {
                u = VT_DOUBLE;
                goto basic_type1;
            }
            break;
        case TOK_ENUM:
            struct_decl(&type1, VT_ENUM);
        basic_type2:
            u = type1.t;
            type->ref = type1.ref;
            goto basic_type1;
        case TOK_STRUCT:
        case TOK_UNION:
            struct_decl(&type1, VT_STRUCT);
            goto basic_type2;

            /* type modifiers */
        case TOK_CONST1:
        case TOK_CONST2:
        case TOK_CONST3:
            t |= VT_CONSTANT;
            next();
            break;
        case TOK_VOLATILE1:
        case TOK_VOLATILE2:
        case TOK_VOLATILE3:
            t |= VT_VOLATILE;
            next();
            break;
        case TOK_SIGNED1:
        case TOK_SIGNED2:
        case TOK_SIGNED3:
            typespec_found = 1;
            t |= VT_SIGNED;
            next();
            break;
        case TOK_REGISTER:
        case TOK_AUTO:
        case TOK_RESTRICT1:
        case TOK_RESTRICT2:
        case TOK_RESTRICT3:
            next();
            break;
        case TOK_UNSIGNED:
            t |= VT_UNSIGNED;
            next();
            typespec_found = 1;
            break;

            /* storage */
        case TOK_EXTERN:
            t |= VT_EXTERN;
            next();
            break;
        case TOK_STATIC:
            t |= VT_STATIC;
            next();
            break;
        case TOK_TYPEDEF:
            t |= VT_TYPEDEF;
            next();
            break;
        case TOK_INLINE1:
        case TOK_INLINE2:
        case TOK_INLINE3:
            t |= VT_INLINE;
            next();
            break;

            /* GNUC attribute */
        case TOK_ATTRIBUTE1:
        case TOK_ATTRIBUTE2:
            parse_attribute(ad);
            break;
            /* GNUC typeof */
        case TOK_TYPEOF1:
        case TOK_TYPEOF2:
        case TOK_TYPEOF3:
            next();
            parse_expr_type(&type1);
            goto basic_type2;
        default:
            if (typespec_found || typedef_found)
                goto the_end;
            s = sym_find(tok);
            if (!s || !(s->type.t & VT_TYPEDEF))
                goto the_end;
            typedef_found = 1;
            t |= (s->type.t & ~VT_TYPEDEF);
            type->ref = s->type.ref;
            next();
            typespec_found = 1;
            break;
        }
        type_found = 1;
    }
the_end:
    if ((t & (VT_SIGNED|VT_UNSIGNED)) == (VT_SIGNED|VT_UNSIGNED))
        error("signed and unsigned modifier");
    if (tcc_state->char_is_unsigned) {
        if ((t & (VT_SIGNED|VT_UNSIGNED|VT_BTYPE)) == VT_BYTE)
            t |= VT_UNSIGNED;
    }
    t &= ~VT_SIGNED;

    /* long is never used as type */
    if ((t & VT_BTYPE) == VT_LONG)
#ifndef TCC_TARGET_X86_64
        t = (t & ~VT_BTYPE) | VT_INT;
#else
        t = (t & ~VT_BTYPE) | VT_LLONG;
#endif
    type->t = t;
    return type_found;
}

/* convert a function parameter type (array to pointer and function to
   function pointer) */
static inline void convert_parameter_type(CType *pt)
{
    /* remove const and volatile qualifiers (XXX: const could be used
       to indicate a const function parameter */
    pt->t &= ~(VT_CONSTANT | VT_VOLATILE);
    /* array must be transformed to pointer according to ANSI C */
    pt->t &= ~VT_ARRAY;
    if ((pt->t & VT_BTYPE) == VT_FUNC) {
        mk_pointer(pt);
    }
}

static void post_type(CType *type, AttributeDef *ad)
{
    int n, l, t1, arg_size, align;
    Sym **plast, *s, *first;
    AttributeDef ad1;
    CType pt;

    if (tok == '(') {
        /* function declaration */
        next();
        l = 0;
        first = NULL;
        plast = &first;
        arg_size = 0;
        if (tok != ')') {
            for(;;) {
                /* read param name and compute offset */
                if (l != FUNC_OLD) {
                    if (!parse_btype(&pt, &ad1)) {
                        if (l) {
                            error("invalid type");
                        } else {
                            l = FUNC_OLD;
                            goto old_proto;
                        }
                    }
                    l = FUNC_NEW;
                    if ((pt.t & VT_BTYPE) == VT_VOID && tok == ')')
                        break;
                    type_decl(&pt, &ad1, &n, TYPE_DIRECT | TYPE_ABSTRACT);
                    if ((pt.t & VT_BTYPE) == VT_VOID)
                        error("parameter declared as void");
                    arg_size += (type_size(&pt, &align) + 3) & ~3;
                } else {
                old_proto:
                    n = tok;
                    if (n < TOK_UIDENT)
                        expect("identifier");
                    pt.t = VT_INT;
                    next();
                }
                convert_parameter_type(&pt);
                s = sym_push(n | SYM_FIELD, &pt, 0, 0);
                *plast = s;
                plast = &s->next;
                if (tok == ')')
                    break;
                skip(',');
                if (l == FUNC_NEW && tok == TOK_DOTS) {
                    l = FUNC_ELLIPSIS;
                    next();
                    break;
                }
            }
        }
        /* if no parameters, then old type prototype */
        if (l == 0)
            l = FUNC_OLD;
        skip(')');
        t1 = type->t & VT_STORAGE;
        /* NOTE: const is ignored in returned type as it has a special
           meaning in gcc / C++ */
        type->t &= ~(VT_STORAGE | VT_CONSTANT); 
        post_type(type, ad);
        /* we push a anonymous symbol which will contain the function prototype */
        FUNC_ARGS(ad->func_attr) = arg_size;
        s = sym_push(SYM_FIELD, type, ad->func_attr, l);
        s->next = first;
        type->t = t1 | VT_FUNC;
        type->ref = s;
    } else if (tok == '[') {
        /* array definition */
        next();
	if (tok == TOK_RESTRICT1)
	    next();
        n = -1;
        if (tok != ']') {
            n = expr_const();
            if (n < 0)
                error("invalid array size");    
        }
        skip(']');
        /* parse next post type */
        t1 = type->t & VT_STORAGE;
        type->t &= ~VT_STORAGE;
        post_type(type, ad);
        
        /* we push a anonymous symbol which will contain the array
           element type */
        s = sym_push(SYM_FIELD, type, 0, n);
        type->t = t1 | VT_ARRAY | VT_PTR;
        type->ref = s;
    }
}

/* Parse a type declaration (except basic type), and return the type
   in 'type'. 'td' is a bitmask indicating which kind of type decl is
   expected. 'type' should contain the basic type. 'ad' is the
   attribute definition of the basic type. It can be modified by
   type_decl(). 
 */
static void type_decl(CType *type, AttributeDef *ad, int *v, int td)
{
    Sym *s;
    CType type1, *type2;
    int qualifiers;
    
    while (tok == '*') {
        qualifiers = 0;
    redo:
        next();
        switch(tok) {
        case TOK_CONST1:
        case TOK_CONST2:
        case TOK_CONST3:
            qualifiers |= VT_CONSTANT;
            goto redo;
        case TOK_VOLATILE1:
        case TOK_VOLATILE2:
        case TOK_VOLATILE3:
            qualifiers |= VT_VOLATILE;
            goto redo;
        case TOK_RESTRICT1:
        case TOK_RESTRICT2:
        case TOK_RESTRICT3:
            goto redo;
        }
        mk_pointer(type);
        type->t |= qualifiers;
    }
    
    /* XXX: clarify attribute handling */
    if (tok == TOK_ATTRIBUTE1 || tok == TOK_ATTRIBUTE2)
        parse_attribute(ad);

    /* recursive type */
    /* XXX: incorrect if abstract type for functions (e.g. 'int ()') */
    type1.t = 0; /* XXX: same as int */
    if (tok == '(') {
        next();
        /* XXX: this is not correct to modify 'ad' at this point, but
           the syntax is not clear */
        if (tok == TOK_ATTRIBUTE1 || tok == TOK_ATTRIBUTE2)
            parse_attribute(ad);
        type_decl(&type1, ad, v, td);
        skip(')');
    } else {
        /* type identifier */
        if (tok >= TOK_IDENT && (td & TYPE_DIRECT)) {
            *v = tok;
            next();
        } else {
            if (!(td & TYPE_ABSTRACT))
                expect("identifier");
            *v = 0;
        }
    }
    post_type(type, ad);
    if (tok == TOK_ATTRIBUTE1 || tok == TOK_ATTRIBUTE2)
        parse_attribute(ad);
    if (!type1.t)
        return;
    /* append type at the end of type1 */
    type2 = &type1;
    for(;;) {
        s = type2->ref;
        type2 = &s->type;
        if (!type2->t) {
            *type2 = *type;
            break;
        }
    }
    *type = type1;
}

/* compute the lvalue VT_LVAL_xxx needed to match type t. */
int lvalue_type(int t)
{
    int bt, r;
    r = VT_LVAL;
    bt = t & VT_BTYPE;
    if (bt == VT_BYTE || bt == VT_BOOL)
        r |= VT_LVAL_BYTE;
    else if (bt == VT_SHORT)
        r |= VT_LVAL_SHORT;
    else
        return r;
    if (t & VT_UNSIGNED)
        r |= VT_LVAL_UNSIGNED;
    return r;
}

/* indirection with full error checking and bound check */
static void indir(void)
{
    if ((vtop->type.t & VT_BTYPE) != VT_PTR) {
        if ((vtop->type.t & VT_BTYPE) == VT_FUNC)
            return;
        expect("pointer");
    }
    if ((vtop->r & VT_LVAL) && !nocode_wanted)
        gv(RC_INT);
    vtop->type = *pointed_type(&vtop->type);
    /* Arrays and functions are never lvalues */
    if (!(vtop->type.t & VT_ARRAY)
        && (vtop->type.t & VT_BTYPE) != VT_FUNC) {
        vtop->r |= lvalue_type(vtop->type.t);
        /* if bound checking, the referenced pointer must be checked */
#ifdef CONFIG_TCC_BCHECK
        if (do_bounds_check) 
            vtop->r |= VT_MUSTBOUND;
#endif
    }
}

/* pass a parameter to a function and do type checking and casting */
static void gfunc_param_typed(Sym *func, Sym *arg)
{
    int func_type;
    CType type;

    func_type = func->c;
    if (func_type == FUNC_OLD ||
        (func_type == FUNC_ELLIPSIS && arg == NULL)) {
        /* default casting : only need to convert float to double */
        if ((vtop->type.t & VT_BTYPE) == VT_FLOAT) {
            type.t = VT_DOUBLE;
            gen_cast(&type);
        }
    } else if (arg == NULL) {
        error("too many arguments to function");
    } else {
        type = arg->type;
        type.t &= ~VT_CONSTANT; /* need to do that to avoid false warning */
        gen_assign_cast(&type);
    }
}

/* parse an expression of the form '(type)' or '(expr)' and return its
   type */
static void parse_expr_type(CType *type)
{
    int n;
    AttributeDef ad;

    skip('(');
    if (parse_btype(type, &ad)) {
        type_decl(type, &ad, &n, TYPE_ABSTRACT);
    } else {
        expr_type(type);
    }
    skip(')');
}

static void parse_type(CType *type)
{
    AttributeDef ad;
    int n;

    if (!parse_btype(type, &ad)) {
        expect("type");
    }
    type_decl(type, &ad, &n, TYPE_ABSTRACT);
}

static void vpush_tokc(int t)
{
    CType type;
    type.t = t;
    vsetc(&type, VT_CONST, &tokc);
}

static void unary(void)
{
    int n, t, align, size, r;
    CType type;
    Sym *s;
    AttributeDef ad;

    /* XXX: GCC 2.95.3 does not generate a table although it should be
       better here */
 tok_next:
    switch(tok) {
    case TOK_EXTENSION:
        next();
        goto tok_next;
    case TOK_CINT:
    case TOK_CCHAR: 
    case TOK_LCHAR:
        vpushi(tokc.i);
        next();
        break;
    case TOK_CUINT:
        vpush_tokc(VT_INT | VT_UNSIGNED);
        next();
        break;
    case TOK_CLLONG:
        vpush_tokc(VT_LLONG);
        next();
        break;
    case TOK_CULLONG:
        vpush_tokc(VT_LLONG | VT_UNSIGNED);
        next();
        break;
    case TOK_CFLOAT:
        vpush_tokc(VT_FLOAT);
        next();
        break;
    case TOK_CDOUBLE:
        vpush_tokc(VT_DOUBLE);
        next();
        break;
    case TOK_CLDOUBLE:
        vpush_tokc(VT_LDOUBLE);
        next();
        break;
    case TOK___FUNCTION__:
        if (!gnu_ext)
            goto tok_identifier;
        /* fall thru */
    case TOK___FUNC__:
        {
            void *ptr;
            int len;
            /* special function name identifier */
            len = strlen(funcname) + 1;
            /* generate char[len] type */
            type.t = VT_BYTE;
            mk_pointer(&type);
            type.t |= VT_ARRAY;
            type.ref->c = len;
            vpush_ref(&type, data_section, data_section->data_offset, len);
            ptr = section_ptr_add(data_section, len);
            memcpy(ptr, funcname, len);
            next();
        }
        break;
    case TOK_LSTR:
#ifdef TCC_TARGET_PE
        t = VT_SHORT | VT_UNSIGNED;
#else
        t = VT_INT;
#endif
        goto str_init;
    case TOK_STR:
        /* string parsing */
        t = VT_BYTE;
    str_init:
        if (tcc_state->warn_write_strings)
            t |= VT_CONSTANT;
        type.t = t;
        mk_pointer(&type);
        type.t |= VT_ARRAY;
        memset(&ad, 0, sizeof(AttributeDef));
        decl_initializer_alloc(&type, &ad, VT_CONST, 2, 0, 0);
        break;
    case '(':
        next();
        /* cast ? */
        if (parse_btype(&type, &ad)) {
            type_decl(&type, &ad, &n, TYPE_ABSTRACT);
            skip(')');
            /* check ISOC99 compound literal */
            if (tok == '{') {
                    /* data is allocated locally by default */
                if (global_expr)
                    r = VT_CONST;
                else
                    r = VT_LOCAL;
                /* all except arrays are lvalues */
                if (!(type.t & VT_ARRAY))
                    r |= lvalue_type(type.t);
                memset(&ad, 0, sizeof(AttributeDef));
                decl_initializer_alloc(&type, &ad, r, 1, 0, 0);
            } else {
                unary();
                gen_cast(&type);
            }
        } else if (tok == '{') {
            /* save all registers */
            save_regs(0); 
            /* statement expression : we do not accept break/continue
               inside as GCC does */
            block(NULL, NULL, NULL, NULL, 0, 1);
            skip(')');
        } else {
            gexpr();
            skip(')');
        }
        break;
    case '*':
        next();
        unary();
        indir();
        break;
    case '&':
        next();
        unary();
        /* functions names must be treated as function pointers,
           except for unary '&' and sizeof. Since we consider that
           functions are not lvalues, we only have to handle it
           there and in function calls. */
        /* arrays can also be used although they are not lvalues */
        if ((vtop->type.t & VT_BTYPE) != VT_FUNC &&
            !(vtop->type.t & VT_ARRAY) && !(vtop->type.t & VT_LLOCAL))
            test_lvalue();
        mk_pointer(&vtop->type);
        gaddrof();
        break;
    case '!':
        next();
        unary();
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
            CType boolean;
            boolean.t = VT_BOOL;
            gen_cast(&boolean);
            vtop->c.i = !vtop->c.i;
        } else if ((vtop->r & VT_VALMASK) == VT_CMP)
            vtop->c.i = vtop->c.i ^ 1;
        else {
            save_regs(1);
            vseti(VT_JMP, gtst(1, 0));
        }
        break;
    case '~':
        next();
        unary();
        vpushi(-1);
        gen_op('^');
        break;
    case '+':
        next();
        /* in order to force cast, we add zero */
        unary();
        if ((vtop->type.t & VT_BTYPE) == VT_PTR)
            error("pointer not accepted for unary plus");
        vpushi(0);
        gen_op('+');
        break;
    case TOK_SIZEOF:
    case TOK_ALIGNOF1:
    case TOK_ALIGNOF2:
        t = tok;
        next();
        if (tok == '(') {
            parse_expr_type(&type);
        } else {
            unary_type(&type);
        }
        size = type_size(&type, &align);
        if (t == TOK_SIZEOF) {
            if (size < 0)
                error("sizeof applied to an incomplete type");
            vpushi(size);
        } else {
            vpushi(align);
        }
        vtop->type.t |= VT_UNSIGNED;
        break;

    case TOK_builtin_types_compatible_p:
        {
            CType type1, type2;
            next();
            skip('(');
            parse_type(&type1);
            skip(',');
            parse_type(&type2);
            skip(')');
            type1.t &= ~(VT_CONSTANT | VT_VOLATILE);
            type2.t &= ~(VT_CONSTANT | VT_VOLATILE);
            vpushi(is_compatible_types(&type1, &type2));
        }
        break;
    case TOK_builtin_constant_p:
        {
            int saved_nocode_wanted, res;
            next();
            skip('(');
            saved_nocode_wanted = nocode_wanted;
            nocode_wanted = 1;
            gexpr();
            res = (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
            vpop();
            nocode_wanted = saved_nocode_wanted;
            skip(')');
            vpushi(res);
        }
        break;
    case TOK_builtin_frame_address:
        {
            CType type;
            next();
            skip('(');
            if (tok != TOK_CINT) {
                error("__builtin_frame_address only takes integers");
            }
            if (tokc.i != 0) {
                error("TCC only supports __builtin_frame_address(0)");
            }
            next();
            skip(')');
            type.t = VT_VOID;
            mk_pointer(&type);
            vset(&type, VT_LOCAL, 0);
        }
        break;
#ifdef TCC_TARGET_X86_64
    case TOK_builtin_malloc:
        tok = TOK_malloc;
        goto tok_identifier;
    case TOK_builtin_free:
        tok = TOK_free;
        goto tok_identifier;
#endif
    case TOK_INC:
    case TOK_DEC:
        t = tok;
        next();
        unary();
        inc(0, t);
        break;
    case '-':
        next();
        vpushi(0);
        unary();
        gen_op('-');
        break;
    case TOK_LAND:
        if (!gnu_ext)
            goto tok_identifier;
        next();
        /* allow to take the address of a label */
        if (tok < TOK_UIDENT)
            expect("label identifier");
        s = label_find(tok);
        if (!s) {
            s = label_push(&global_label_stack, tok, LABEL_FORWARD);
        } else {
            if (s->r == LABEL_DECLARED)
                s->r = LABEL_FORWARD;
        }
        if (!s->type.t) {
            s->type.t = VT_VOID;
            mk_pointer(&s->type);
            s->type.t |= VT_STATIC;
        }
        vset(&s->type, VT_CONST | VT_SYM, 0);
        vtop->sym = s;
        next();
        break;
    default:
    tok_identifier:
        t = tok;
        next();
        if (t < TOK_UIDENT)
            expect("identifier");
        s = sym_find(t);
        if (!s) {
            if (tok != '(')
                error("'%s' undeclared", get_tok_str(t, NULL));
            /* for simple function calls, we tolerate undeclared
               external reference to int() function */
            if (tcc_state->warn_implicit_function_declaration)
                warning("implicit declaration of function '%s'",
                        get_tok_str(t, NULL));
            s = external_global_sym(t, &func_old_type, 0); 
        }
        if ((s->type.t & (VT_STATIC | VT_INLINE | VT_BTYPE)) ==
            (VT_STATIC | VT_INLINE | VT_FUNC)) {
            /* if referencing an inline function, then we generate a
               symbol to it if not already done. It will have the
               effect to generate code for it at the end of the
               compilation unit. Inline function as always
               generated in the text section. */
            if (!s->c)
                put_extern_sym(s, text_section, 0, 0);
            r = VT_SYM | VT_CONST;
        } else {
            r = s->r;
        }
        vset(&s->type, r, s->c);
        /* if forward reference, we must point to s */
        if (vtop->r & VT_SYM) {
            vtop->sym = s;
            vtop->c.ul = 0;
        }
        break;
    }
    
    /* post operations */
    while (1) {
        if (tok == TOK_INC || tok == TOK_DEC) {
            inc(1, tok);
            next();
        } else if (tok == '.' || tok == TOK_ARROW) {
            /* field */ 
            if (tok == TOK_ARROW) 
                indir();
            test_lvalue();
            gaddrof();
            next();
            /* expect pointer on structure */
            if ((vtop->type.t & VT_BTYPE) != VT_STRUCT)
                expect("struct or union");
            s = vtop->type.ref;
            /* find field */
            tok |= SYM_FIELD;
            while ((s = s->next) != NULL) {
                if (s->v == tok)
                    break;
            }
            if (!s)
                error("field not found: %s",  get_tok_str(tok & ~SYM_FIELD, NULL));
            /* add field offset to pointer */
            vtop->type = char_pointer_type; /* change type to 'char *' */
            vpushi(s->c);
            gen_op('+');
            /* change type to field type, and set to lvalue */
            vtop->type = s->type;
            /* an array is never an lvalue */
            if (!(vtop->type.t & VT_ARRAY)) {
                vtop->r |= lvalue_type(vtop->type.t);
                /* if bound checking, the referenced pointer must be checked */
#ifdef CONFIG_TCC_BCHECK
                if (do_bounds_check) 
                    vtop->r |= VT_MUSTBOUND;
#endif
            }
            next();
        } else if (tok == '[') {
            next();
            gexpr();
            gen_op('+');
            indir();
            skip(']');
        } else if (tok == '(') {
            SValue ret;
            Sym *sa;
            int nb_args;

            /* function call  */
            if ((vtop->type.t & VT_BTYPE) != VT_FUNC) {
                /* pointer test (no array accepted) */
                if ((vtop->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR) {
                    vtop->type = *pointed_type(&vtop->type);
                    if ((vtop->type.t & VT_BTYPE) != VT_FUNC)
                        goto error_func;
                } else {
                error_func:
                    expect("function pointer");
                }
            } else {
                vtop->r &= ~VT_LVAL; /* no lvalue */
            }
            /* get return type */
            s = vtop->type.ref;
            next();
            sa = s->next; /* first parameter */
            nb_args = 0;
            ret.r2 = VT_CONST;
            /* compute first implicit argument if a structure is returned */
            if ((s->type.t & VT_BTYPE) == VT_STRUCT) {
                /* get some space for the returned structure */
                size = type_size(&s->type, &align);
                loc = (loc - size) & -align;
                ret.type = s->type;
                ret.r = VT_LOCAL | VT_LVAL;
                /* pass it as 'int' to avoid structure arg passing
                   problems */
                vseti(VT_LOCAL, loc);
                ret.c = vtop->c;
                nb_args++;
            } else {
                ret.type = s->type; 
                /* return in register */
                if (is_float(ret.type.t)) {
                    ret.r = reg_fret(ret.type.t);
                } else {
                    if ((ret.type.t & VT_BTYPE) == VT_LLONG)
                        ret.r2 = REG_LRET;
                    ret.r = REG_IRET;
                }
                ret.c.i = 0;
            }
            if (tok != ')') {
                for(;;) {
                    expr_eq();
                    gfunc_param_typed(s, sa);
                    nb_args++;
                    if (sa)
                        sa = sa->next;
                    if (tok == ')')
                        break;
                    skip(',');
                }
            }
            if (sa)
                error("too few arguments to function");
            skip(')');
            if (!nocode_wanted) {
                gfunc_call(nb_args);
            } else {
                vtop -= (nb_args + 1);
            }
            /* return value */
            vsetc(&ret.type, ret.r, &ret.c);
            vtop->r2 = ret.r2;
        } else {
            break;
        }
    }
}

static void uneq(void)
{
    int t;
    
    unary();
    if (tok == '=' ||
        (tok >= TOK_A_MOD && tok <= TOK_A_DIV) ||
        tok == TOK_A_XOR || tok == TOK_A_OR ||
        tok == TOK_A_SHL || tok == TOK_A_SAR) {
        test_lvalue();
        t = tok;
        next();
        if (t == '=') {
            expr_eq();
        } else {
            vdup();
            expr_eq();
            gen_op(t & 0x7f);
        }
        vstore();
    }
}

static void expr_prod(void)
{
    int t;

    uneq();
    while (tok == '*' || tok == '/' || tok == '%') {
        t = tok;
        next();
        uneq();
        gen_op(t);
    }
}

static void expr_sum(void)
{
    int t;

    expr_prod();
    while (tok == '+' || tok == '-') {
        t = tok;
        next();
        expr_prod();
        gen_op(t);
    }
}

static void expr_shift(void)
{
    int t;

    expr_sum();
    while (tok == TOK_SHL || tok == TOK_SAR) {
        t = tok;
        next();
        expr_sum();
        gen_op(t);
    }
}

static void expr_cmp(void)
{
    int t;

    expr_shift();
    while ((tok >= TOK_ULE && tok <= TOK_GT) ||
           tok == TOK_ULT || tok == TOK_UGE) {
        t = tok;
        next();
        expr_shift();
        gen_op(t);
    }
}

static void expr_cmpeq(void)
{
    int t;

    expr_cmp();
    while (tok == TOK_EQ || tok == TOK_NE) {
        t = tok;
        next();
        expr_cmp();
        gen_op(t);
    }
}

static void expr_and(void)
{
    expr_cmpeq();
    while (tok == '&') {
        next();
        expr_cmpeq();
        gen_op('&');
    }
}

static void expr_xor(void)
{
    expr_and();
    while (tok == '^') {
        next();
        expr_and();
        gen_op('^');
    }
}

static void expr_or(void)
{
    expr_xor();
    while (tok == '|') {
        next();
        expr_xor();
        gen_op('|');
    }
}

/* XXX: fix this mess */
static void expr_land_const(void)
{
    expr_or();
    while (tok == TOK_LAND) {
        next();
        expr_or();
        gen_op(TOK_LAND);
    }
}

/* XXX: fix this mess */
static void expr_lor_const(void)
{
    expr_land_const();
    while (tok == TOK_LOR) {
        next();
        expr_land_const();
        gen_op(TOK_LOR);
    }
}

/* only used if non constant */
static void expr_land(void)
{
    int t;

    expr_or();
    if (tok == TOK_LAND) {
        t = 0;
        save_regs(1);
        for(;;) {
            t = gtst(1, t);
            if (tok != TOK_LAND) {
                vseti(VT_JMPI, t);
                break;
            }
            next();
            expr_or();
        }
    }
}

static void expr_lor(void)
{
    int t;

    expr_land();
    if (tok == TOK_LOR) {
        t = 0;
        save_regs(1);
        for(;;) {
            t = gtst(0, t);
            if (tok != TOK_LOR) {
                vseti(VT_JMP, t);
                break;
            }
            next();
            expr_land();
        }
    }
}

/* XXX: better constant handling */
static void expr_eq(void)
{
    int tt, u, r1, r2, rc, t1, t2, bt1, bt2;
    SValue sv;
    CType type, type1, type2;

    if (const_wanted) {
        expr_lor_const();
        if (tok == '?') {
            CType boolean;
            int c;
            boolean.t = VT_BOOL;
            vdup();
            gen_cast(&boolean);
            c = vtop->c.i;
            vpop();
            next();
            if (tok != ':' || !gnu_ext) {
                vpop();
                gexpr();
            }
            if (!c)
                vpop();
            skip(':');
            expr_eq();
            if (c)
                vpop();
        }
    } else {
        expr_lor();
        if (tok == '?') {
            next();
            if (vtop != vstack) {
                /* needed to avoid having different registers saved in
                   each branch */
                if (is_float(vtop->type.t)) {
                    rc = RC_FLOAT;
#ifdef TCC_TARGET_X86_64
                    if ((vtop->type.t & VT_BTYPE) == VT_LDOUBLE) {
                        rc = RC_ST0;
                    }
#endif
                }
                else
                    rc = RC_INT;
                    gv(rc);
                    save_regs(1);
            }
            if (tok == ':' && gnu_ext) {
                gv_dup();
                tt = gtst(1, 0);
            } else {
                tt = gtst(1, 0);
                gexpr();
            }
            type1 = vtop->type;
            sv = *vtop; /* save value to handle it later */
            vtop--; /* no vpop so that FP stack is not flushed */
            skip(':');
            u = gjmp(0);
            gsym(tt);
            expr_eq();
            type2 = vtop->type;

            t1 = type1.t;
            bt1 = t1 & VT_BTYPE;
            t2 = type2.t;
            bt2 = t2 & VT_BTYPE;
            /* cast operands to correct type according to ISOC rules */
            if (is_float(bt1) || is_float(bt2)) {
                if (bt1 == VT_LDOUBLE || bt2 == VT_LDOUBLE) {
                    type.t = VT_LDOUBLE;
                } else if (bt1 == VT_DOUBLE || bt2 == VT_DOUBLE) {
                    type.t = VT_DOUBLE;
                } else {
                    type.t = VT_FLOAT;
                }
            } else if (bt1 == VT_LLONG || bt2 == VT_LLONG) {
                /* cast to biggest op */
                type.t = VT_LLONG;
                /* convert to unsigned if it does not fit in a long long */
                if ((t1 & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED) ||
                    (t2 & (VT_BTYPE | VT_UNSIGNED)) == (VT_LLONG | VT_UNSIGNED))
                    type.t |= VT_UNSIGNED;
            } else if (bt1 == VT_PTR || bt2 == VT_PTR) {
                /* XXX: test pointer compatibility */
                type = type1;
            } else if (bt1 == VT_FUNC || bt2 == VT_FUNC) {
                /* XXX: test function pointer compatibility */
                type = type1;
            } else if (bt1 == VT_STRUCT || bt2 == VT_STRUCT) {
                /* XXX: test structure compatibility */
                type = type1;
            } else if (bt1 == VT_VOID || bt2 == VT_VOID) {
                /* NOTE: as an extension, we accept void on only one side */
                type.t = VT_VOID;
            } else {
                /* integer operations */
                type.t = VT_INT;
                /* convert to unsigned if it does not fit in an integer */
                if ((t1 & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED) ||
                    (t2 & (VT_BTYPE | VT_UNSIGNED)) == (VT_INT | VT_UNSIGNED))
                    type.t |= VT_UNSIGNED;
            }
                
            /* now we convert second operand */
            gen_cast(&type);
            if (VT_STRUCT == (vtop->type.t & VT_BTYPE))
                gaddrof();
            rc = RC_INT;
            if (is_float(type.t)) {
                rc = RC_FLOAT;
#ifdef TCC_TARGET_X86_64
                if ((type.t & VT_BTYPE) == VT_LDOUBLE) {
                    rc = RC_ST0;
                }
#endif
            } else if ((type.t & VT_BTYPE) == VT_LLONG) {
                /* for long longs, we use fixed registers to avoid having
                   to handle a complicated move */
                rc = RC_IRET; 
            }
            
            r2 = gv(rc);
            /* this is horrible, but we must also convert first
               operand */
            tt = gjmp(0);
            gsym(u);
            /* put again first value and cast it */
            *vtop = sv;
            gen_cast(&type);
            if (VT_STRUCT == (vtop->type.t & VT_BTYPE))
                gaddrof();
            r1 = gv(rc);
            move_reg(r2, r1);
            vtop->r = r2;
            gsym(tt);
        }
    }
}

void gexpr(void)
{
    while (1) {
        expr_eq();
        if (tok != ',')
            break;
        vpop();
        next();
    }
}

/* parse an expression and return its type without any side effect. */
static void expr_type(CType *type)
{
    int saved_nocode_wanted;

    saved_nocode_wanted = nocode_wanted;
    nocode_wanted = 1;
    gexpr();
    *type = vtop->type;
    vpop();
    nocode_wanted = saved_nocode_wanted;
}

/* parse a unary expression and return its type without any side
   effect. */
static void unary_type(CType *type)
{
    int a;

    a = nocode_wanted;
    nocode_wanted = 1;
    unary();
    *type = vtop->type;
    vpop();
    nocode_wanted = a;
}

/* parse a constant expression and return value in vtop.  */
static void expr_const1(void)
{
    int a;
    a = const_wanted;
    const_wanted = 1;
    expr_eq();
    const_wanted = a;
}

/* parse an integer constant and return its value. */
int expr_const(void)
{
    int c;
    expr_const1();
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
        expect("constant expression");
    c = vtop->c.i;
    vpop();
    return c;
}

/* return the label token if current token is a label, otherwise
   return zero */
static int is_label(void)
{
    int last_tok;

    /* fast test first */
    if (tok < TOK_UIDENT)
        return 0;
    /* no need to save tokc because tok is an identifier */
    last_tok = tok;
    next();
    if (tok == ':') {
        next();
        return last_tok;
    } else {
        unget_tok(last_tok);
        return 0;
    }
}

static void block(int *bsym, int *csym, int *case_sym, int *def_sym, 
                  int case_reg, int is_expr)
{
    int a, b, c, d;
    Sym *s;

    /* generate line number info */
#ifdef CONFIG_TCC_DEBUG
    if (do_debug && 
        (last_line_num != file->line_num || last_ind != ind)) {
        put_stabn(N_SLINE, 0, file->line_num, ind - func_ind);
        last_ind = ind;
        last_line_num = file->line_num;
    }
#endif

    if (is_expr) {
        /* default return value is (void) */
        vpushi(0);
        vtop->type.t = VT_VOID;
    }

    if (tok == TOK_IF) {
        /* if test */
        next();
        skip('(');
        gexpr();
        skip(')');
        a = gtst(1, 0);
        block(bsym, csym, case_sym, def_sym, case_reg, 0);
        c = tok;
        if (c == TOK_ELSE) {
            next();
            d = gjmp(0);
            gsym(a);
            block(bsym, csym, case_sym, def_sym, case_reg, 0);
            gsym(d); /* patch else jmp */
        } else
            gsym(a);
    } else if (tok == TOK_WHILE) {
        next();
        d = ind;
        skip('(');
        gexpr();
        skip(')');
        a = gtst(1, 0);
        b = 0;
        block(&a, &b, case_sym, def_sym, case_reg, 0);
        gjmp_addr(d);
        gsym(a);
        gsym_addr(b, d);
    } else if (tok == '{') {
        Sym *llabel;
        
        next();
        /* record local declaration stack position */
        s = local_stack;
        llabel = local_label_stack;
        /* handle local labels declarations */
        if (tok == TOK_LABEL) {
            next();
            for(;;) {
                if (tok < TOK_UIDENT)
                    expect("label identifier");
                label_push(&local_label_stack, tok, LABEL_DECLARED);
                next();
                if (tok == ',') {
                    next();
                } else {
                    skip(';');
                    break;
                }
            }
        }
        while (tok != '}') {
            decl(VT_LOCAL);
            if (tok != '}') {
                if (is_expr)
                    vpop();
                block(bsym, csym, case_sym, def_sym, case_reg, is_expr);
            }
        }
        /* pop locally defined labels */
        label_pop(&local_label_stack, llabel);
        /* pop locally defined symbols */
        if(is_expr) {
            /* XXX: this solution makes only valgrind happy...
               triggered by gcc.c-torture/execute/20000917-1.c */
            Sym *p;
            switch(vtop->type.t & VT_BTYPE) {
            case VT_PTR:
            case VT_STRUCT:
            case VT_ENUM:
            case VT_FUNC:
                for(p=vtop->type.ref;p;p=p->prev)
                    if(p->prev==s)
                        error("unsupported expression type");
            }
        }
        sym_pop(&local_stack, s);
        next();
    } else if (tok == TOK_RETURN) {
        next();
        if (tok != ';') {
            gexpr();
            gen_assign_cast(&func_vt);
            if ((func_vt.t & VT_BTYPE) == VT_STRUCT) {
                CType type;
                /* if returning structure, must copy it to implicit
                   first pointer arg location */
#ifdef TCC_ARM_EABI
                int align, size;
                size = type_size(&func_vt,&align);
                if(size <= 4)
                {
                    if((vtop->r != (VT_LOCAL | VT_LVAL) || (vtop->c.i & 3))
                       && (align & 3))
                    {
                        int addr;
                        loc = (loc - size) & -4;
                        addr = loc;
                        type = func_vt;
                        vset(&type, VT_LOCAL | VT_LVAL, addr);
                        vswap();
                        vstore();
                        vset(&int_type, VT_LOCAL | VT_LVAL, addr);
                    }
                    vtop->type = int_type;
                    gv(RC_IRET);
                } else {
#endif
                type = func_vt;
                mk_pointer(&type);
                vset(&type, VT_LOCAL | VT_LVAL, func_vc);
                indir();
                vswap();
                /* copy structure value to pointer */
                vstore();
#ifdef TCC_ARM_EABI
                }
#endif
            } else if (is_float(func_vt.t)) {
                gv(rc_fret(func_vt.t));
            } else {
                gv(RC_IRET);
            }
            vtop--; /* NOT vpop() because on x86 it would flush the fp stack */
        }
        skip(';');
        rsym = gjmp(rsym); /* jmp */
    } else if (tok == TOK_BREAK) {
        /* compute jump */
        if (!bsym)
            error("cannot break");
        *bsym = gjmp(*bsym);
        next();
        skip(';');
    } else if (tok == TOK_CONTINUE) {
        /* compute jump */
        if (!csym)
            error("cannot continue");
        *csym = gjmp(*csym);
        next();
        skip(';');
    } else if (tok == TOK_FOR) {
        int e;
        next();
        skip('(');
        if (tok != ';') {
            gexpr();
            vpop();
        }
        skip(';');
        d = ind;
        c = ind;
        a = 0;
        b = 0;
        if (tok != ';') {
            gexpr();
            a = gtst(1, 0);
        }
        skip(';');
        if (tok != ')') {
            e = gjmp(0);
            c = ind;
            gexpr();
            vpop();
            gjmp_addr(d);
            gsym(e);
        }
        skip(')');
        block(&a, &b, case_sym, def_sym, case_reg, 0);
        gjmp_addr(c);
        gsym(a);
        gsym_addr(b, c);
    } else 
    if (tok == TOK_DO) {
        next();
        a = 0;
        b = 0;
        d = ind;
        block(&a, &b, case_sym, def_sym, case_reg, 0);
        skip(TOK_WHILE);
        skip('(');
        gsym(b);
        gexpr();
        c = gtst(0, 0);
        gsym_addr(c, d);
        skip(')');
        gsym(a);
        skip(';');
    } else
    if (tok == TOK_SWITCH) {
        next();
        skip('(');
        gexpr();
        /* XXX: other types than integer */
        case_reg = gv(RC_INT);
        vpop();
        skip(')');
        a = 0;
        b = gjmp(0); /* jump to first case */
        c = 0;
        block(&a, csym, &b, &c, case_reg, 0);
        /* if no default, jmp after switch */
        if (c == 0)
            c = ind;
        /* default label */
        gsym_addr(b, c);
        /* break label */
        gsym(a);
    } else
    if (tok == TOK_CASE) {
        int v1, v2;
        if (!case_sym)
            expect("switch");
        next();
        v1 = expr_const();
        v2 = v1;
        if (gnu_ext && tok == TOK_DOTS) {
            next();
            v2 = expr_const();
            if (v2 < v1)
                warning("empty case range");
        }
        /* since a case is like a label, we must skip it with a jmp */
        b = gjmp(0);
        gsym(*case_sym);
        vseti(case_reg, 0);
        vpushi(v1);
        if (v1 == v2) {
            gen_op(TOK_EQ);
            *case_sym = gtst(1, 0);
        } else {
            gen_op(TOK_GE);
            *case_sym = gtst(1, 0);
            vseti(case_reg, 0);
            vpushi(v2);
            gen_op(TOK_LE);
            *case_sym = gtst(1, *case_sym);
        }
        gsym(b);
        skip(':');
        is_expr = 0;
        goto block_after_label;
    } else 
    if (tok == TOK_DEFAULT) {
        next();
        skip(':');
        if (!def_sym)
            expect("switch");
        if (*def_sym)
            error("too many 'default'");
        *def_sym = ind;
        is_expr = 0;
        goto block_after_label;
    } else
    if (tok == TOK_GOTO) {
        next();
        if (tok == '*' && gnu_ext) {
            /* computed goto */
            next();
            gexpr();
            if ((vtop->type.t & VT_BTYPE) != VT_PTR)
                expect("pointer");
            ggoto();
        } else if (tok >= TOK_UIDENT) {
            s = label_find(tok);
            /* put forward definition if needed */
            if (!s) {
                s = label_push(&global_label_stack, tok, LABEL_FORWARD);
            } else {
                if (s->r == LABEL_DECLARED)
                    s->r = LABEL_FORWARD;
            }
            /* label already defined */
            if (s->r & LABEL_FORWARD) 
                s->next = (void *)gjmp((long)s->next);
            else
                gjmp_addr((long)s->next);
            next();
        } else {
            expect("label identifier");
        }
        skip(';');
    } else if (tok == TOK_ASM1 || tok == TOK_ASM2 || tok == TOK_ASM3) {
        asm_instr();
    } else {
        b = is_label();
        if (b) {
            /* label case */
            s = label_find(b);
            if (s) {
                if (s->r == LABEL_DEFINED)
                    error("duplicate label '%s'", get_tok_str(s->v, NULL));
                gsym((long)s->next);
                s->r = LABEL_DEFINED;
            } else {
                s = label_push(&global_label_stack, b, LABEL_DEFINED);
            }
            s->next = (void *)ind;
            /* we accept this, but it is a mistake */
        block_after_label:
            if (tok == '}') {
                warning("deprecated use of label at end of compound statement");
            } else {
                if (is_expr)
                    vpop();
                block(bsym, csym, case_sym, def_sym, case_reg, is_expr);
            }
        } else {
            /* expression case */
            if (tok != ';') {
                if (is_expr) {
                    vpop();
                    gexpr();
                } else {
                    gexpr();
                    vpop();
                }
            }
            skip(';');
        }
    }
}

/* t is the array or struct type. c is the array or struct
   address. cur_index/cur_field is the pointer to the current
   value. 'size_only' is true if only size info is needed (only used
   in arrays) */
static void decl_designator(CType *type, Section *sec, unsigned long c, 
                            int *cur_index, Sym **cur_field, 
                            int size_only)
{
    Sym *s, *f;
    int notfirst, index, index_last, align, l, nb_elems, elem_size;
    CType type1;

    notfirst = 0;
    elem_size = 0;
    nb_elems = 1;
    if (gnu_ext && (l = is_label()) != 0)
        goto struct_field;
    while (tok == '[' || tok == '.') {
        if (tok == '[') {
            if (!(type->t & VT_ARRAY))
                expect("array type");
            s = type->ref;
            next();
            index = expr_const();
            if (index < 0 || (s->c >= 0 && index >= s->c))
                expect("invalid index");
            if (tok == TOK_DOTS && gnu_ext) {
                next();
                index_last = expr_const();
                if (index_last < 0 || 
                    (s->c >= 0 && index_last >= s->c) ||
                    index_last < index)
                    expect("invalid index");
            } else {
                index_last = index;
            }
            skip(']');
            if (!notfirst)
                *cur_index = index_last;
            type = pointed_type(type);
            elem_size = type_size(type, &align);
            c += index * elem_size;
            /* NOTE: we only support ranges for last designator */
            nb_elems = index_last - index + 1;
            if (nb_elems != 1) {
                notfirst = 1;
                break;
            }
        } else {
            next();
            l = tok;
            next();
        struct_field:
            if ((type->t & VT_BTYPE) != VT_STRUCT)
                expect("struct/union type");
            s = type->ref;
            l |= SYM_FIELD;
            f = s->next;
            while (f) {
                if (f->v == l)
                    break;
                f = f->next;
            }
            if (!f)
                expect("field");
            if (!notfirst)
                *cur_field = f;
            /* XXX: fix this mess by using explicit storage field */
            type1 = f->type;
            type1.t |= (type->t & ~VT_TYPE);
            type = &type1;
            c += f->c;
        }
        notfirst = 1;
    }
    if (notfirst) {
        if (tok == '=') {
            next();
        } else {
            if (!gnu_ext)
                expect("=");
        }
    } else {
        if (type->t & VT_ARRAY) {
            index = *cur_index;
            type = pointed_type(type);
            c += index * type_size(type, &align);
        } else {
            f = *cur_field;
            if (!f)
                error("too many field init");
            /* XXX: fix this mess by using explicit storage field */
            type1 = f->type;
            type1.t |= (type->t & ~VT_TYPE);
            type = &type1;
            c += f->c;
        }
    }
    decl_initializer(type, sec, c, 0, size_only);

    /* XXX: make it more general */
    if (!size_only && nb_elems > 1) {
        unsigned long c_end;
        char *src, *dst;
        int i;

        if (!sec)
            error("range init not supported yet for dynamic storage");
        c_end = c + nb_elems * elem_size;
        if (c_end > sec->data_allocated)
            section_realloc(sec, c_end);
        src = sec->data + c;
        dst = src;
        for(i = 1; i < nb_elems; i++) {
            dst += elem_size;
            memcpy(dst, src, elem_size);
        }
    }
}

#define EXPR_VAL   0
#define EXPR_CONST 1
#define EXPR_ANY   2

/* store a value or an expression directly in global data or in local array */
static void init_putv(CType *type, Section *sec, unsigned long c, 
                      int v, int expr_type)
{
    int saved_global_expr, bt, bit_pos, bit_size;
    void *ptr;
    unsigned long long bit_mask;
    CType dtype;

    switch(expr_type) {
    case EXPR_VAL:
        vpushi(v);
        break;
    case EXPR_CONST:
        /* compound literals must be allocated globally in this case */
        saved_global_expr = global_expr;
        global_expr = 1;
        expr_const1();
        global_expr = saved_global_expr;
        /* NOTE: symbols are accepted */
        if ((vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST)
            error("initializer element is not constant");
        break;
    case EXPR_ANY:
        expr_eq();
        break;
    }
    
    dtype = *type;
    dtype.t &= ~VT_CONSTANT; /* need to do that to avoid false warning */

    if (sec) {
        /* XXX: not portable */
        /* XXX: generate error if incorrect relocation */
        gen_assign_cast(&dtype);
        bt = type->t & VT_BTYPE;
        /* we'll write at most 12 bytes */
        if (c + 12 > sec->data_allocated) {
            section_realloc(sec, c + 12);
        }
        ptr = sec->data + c;
        /* XXX: make code faster ? */
        if (!(type->t & VT_BITFIELD)) {
            bit_pos = 0;
            bit_size = 32;
            bit_mask = -1LL;
        } else {
            bit_pos = (vtop->type.t >> VT_STRUCT_SHIFT) & 0x3f;
            bit_size = (vtop->type.t >> (VT_STRUCT_SHIFT + 6)) & 0x3f;
            bit_mask = (1LL << bit_size) - 1;
        }
        if ((vtop->r & VT_SYM) &&
            (bt == VT_BYTE ||
             bt == VT_SHORT ||
             bt == VT_DOUBLE ||
             bt == VT_LDOUBLE ||
             bt == VT_LLONG ||
             (bt == VT_INT && bit_size != 32)))
            error("initializer element is not computable at load time");
        switch(bt) {
        case VT_BYTE:
            *(char *)ptr |= (vtop->c.i & bit_mask) << bit_pos;
            break;
        case VT_SHORT:
            *(short *)ptr |= (vtop->c.i & bit_mask) << bit_pos;
            break;
        case VT_DOUBLE:
            *(double *)ptr = vtop->c.d;
            break;
        case VT_LDOUBLE:
            *(long double *)ptr = vtop->c.ld;
            break;
        case VT_LLONG:
            *(long long *)ptr |= (vtop->c.ll & bit_mask) << bit_pos;
            break;
        default:
            if (vtop->r & VT_SYM) {
                greloc(sec, vtop->sym, c, R_DATA_32);
            }
            *(int *)ptr |= (vtop->c.i & bit_mask) << bit_pos;
            break;
        }
        vtop--;
    } else {
        vset(&dtype, VT_LOCAL|VT_LVAL, c);
        vswap();
        vstore();
        vpop();
    }
}

/* put zeros for variable based init */
static void init_putz(CType *t, Section *sec, unsigned long c, int size)
{
    if (sec) {
        /* nothing to do because globals are already set to zero */
    } else {
        vpush_global_sym(&func_old_type, TOK_memset);
        vseti(VT_LOCAL, c);
        vpushi(0);
        vpushi(size);
        gfunc_call(3);
    }
}

/* 't' contains the type and storage info. 'c' is the offset of the
   object in section 'sec'. If 'sec' is NULL, it means stack based
   allocation. 'first' is true if array '{' must be read (multi
   dimension implicit array init handling). 'size_only' is true if
   size only evaluation is wanted (only for arrays). */
static void decl_initializer(CType *type, Section *sec, unsigned long c, 
                             int first, int size_only)
{
    int index, array_length, n, no_oblock, nb, parlevel, i;
    int size1, align1, expr_type;
    Sym *s, *f;
    CType *t1;

    if (type->t & VT_ARRAY) {
        s = type->ref;
        n = s->c;
        array_length = 0;
        t1 = pointed_type(type);
        size1 = type_size(t1, &align1);

        no_oblock = 1;
        if ((first && tok != TOK_LSTR && tok != TOK_STR) || 
            tok == '{') {
            skip('{');
            no_oblock = 0;
        }

        /* only parse strings here if correct type (otherwise: handle
           them as ((w)char *) expressions */
        if ((tok == TOK_LSTR && 
#ifdef TCC_TARGET_PE
             (t1->t & VT_BTYPE) == VT_SHORT && (t1->t & VT_UNSIGNED)
#else
             (t1->t & VT_BTYPE) == VT_INT
#endif
            ) || (tok == TOK_STR && (t1->t & VT_BTYPE) == VT_BYTE)) {
            while (tok == TOK_STR || tok == TOK_LSTR) {
                int cstr_len, ch;
                CString *cstr;

                cstr = tokc.cstr;
                /* compute maximum number of chars wanted */
                if (tok == TOK_STR)
                    cstr_len = cstr->size;
                else
                    cstr_len = cstr->size / sizeof(nwchar_t);
                cstr_len--;
                nb = cstr_len;
                if (n >= 0 && nb > (n - array_length))
                    nb = n - array_length;
                if (!size_only) {
                    if (cstr_len > nb)
                        warning("initializer-string for array is too long");
                    /* in order to go faster for common case (char
                       string in global variable, we handle it
                       specifically */
                    if (sec && tok == TOK_STR && size1 == 1) {
                        memcpy(sec->data + c + array_length, cstr->data, nb);
                    } else {
                        for(i=0;i<nb;i++) {
                            if (tok == TOK_STR)
                                ch = ((unsigned char *)cstr->data)[i];
                            else
                                ch = ((nwchar_t *)cstr->data)[i];
                            init_putv(t1, sec, c + (array_length + i) * size1,
                                      ch, EXPR_VAL);
                        }
                    }
                }
                array_length += nb;
                next();
            }
            /* only add trailing zero if enough storage (no
               warning in this case since it is standard) */
            if (n < 0 || array_length < n) {
                if (!size_only) {
                    init_putv(t1, sec, c + (array_length * size1), 0, EXPR_VAL);
                }
                array_length++;
            }
        } else {
            index = 0;
            while (tok != '}') {
                decl_designator(type, sec, c, &index, NULL, size_only);
                if (n >= 0 && index >= n)
                    error("index too large");
                /* must put zero in holes (note that doing it that way
                   ensures that it even works with designators) */
                if (!size_only && array_length < index) {
                    init_putz(t1, sec, c + array_length * size1, 
                              (index - array_length) * size1);
                }
                index++;
                if (index > array_length)
                    array_length = index;
                /* special test for multi dimensional arrays (may not
                   be strictly correct if designators are used at the
                   same time) */
                if (index >= n && no_oblock)
                    break;
                if (tok == '}')
                    break;
                skip(',');
            }
        }
        if (!no_oblock)
            skip('}');
        /* put zeros at the end */
        if (!size_only && n >= 0 && array_length < n) {
            init_putz(t1, sec, c + array_length * size1, 
                      (n - array_length) * size1);
        }
        /* patch type size if needed */
        if (n < 0)
            s->c = array_length;
    } else if ((type->t & VT_BTYPE) == VT_STRUCT &&
               (sec || !first || tok == '{')) {
        int par_count;

        /* NOTE: the previous test is a specific case for automatic
           struct/union init */
        /* XXX: union needs only one init */

        /* XXX: this test is incorrect for local initializers
           beginning with ( without {. It would be much more difficult
           to do it correctly (ideally, the expression parser should
           be used in all cases) */
        par_count = 0;
        if (tok == '(') {
            AttributeDef ad1;
            CType type1;
            next();
            while (tok == '(') {
                par_count++;
                next();
            }
            if (!parse_btype(&type1, &ad1))
                expect("cast");
            type_decl(&type1, &ad1, &n, TYPE_ABSTRACT);
#if 0
            if (!is_assignable_types(type, &type1))
                error("invalid type for cast");
#endif
            skip(')');
        }
        no_oblock = 1;
        if (first || tok == '{') {
            skip('{');
            no_oblock = 0;
        }
        s = type->ref;
        f = s->next;
        array_length = 0;
        index = 0;
        n = s->c;
        while (tok != '}') {
            decl_designator(type, sec, c, NULL, &f, size_only);
            index = f->c;
            if (!size_only && array_length < index) {
                init_putz(type, sec, c + array_length, 
                          index - array_length);
            }
            index = index + type_size(&f->type, &align1);
            if (index > array_length)
                array_length = index;
            f = f->next;
            if (no_oblock && f == NULL)
                break;
            if (tok == '}')
                break;
            skip(',');
        }
        /* put zeros at the end */
        if (!size_only && array_length < n) {
            init_putz(type, sec, c + array_length, 
                      n - array_length);
        }
        if (!no_oblock)
            skip('}');
        while (par_count) {
            skip(')');
            par_count--;
        }
    } else if (tok == '{') {
        next();
        decl_initializer(type, sec, c, first, size_only);
        skip('}');
    } else if (size_only) {
        /* just skip expression */
        parlevel = 0;
        while ((parlevel > 0 || (tok != '}' && tok != ',')) && 
               tok != -1) {
            if (tok == '(')
                parlevel++;
            else if (tok == ')')
                parlevel--;
            next();
        }
    } else {
        /* currently, we always use constant expression for globals
           (may change for scripting case) */
        expr_type = EXPR_CONST;
        if (!sec)
            expr_type = EXPR_ANY;
        init_putv(type, sec, c, 0, expr_type);
    }
}

/* parse an initializer for type 't' if 'has_init' is non zero, and
   allocate space in local or global data space ('r' is either
   VT_LOCAL or VT_CONST). If 'v' is non zero, then an associated
   variable 'v' of scope 'scope' is declared before initializers are
   parsed. If 'v' is zero, then a reference to the new object is put
   in the value stack. If 'has_init' is 2, a special parsing is done
   to handle string constants. */
static void decl_initializer_alloc(CType *type, AttributeDef *ad, int r, 
                                   int has_init, int v, int scope)
{
    int size, align, addr, data_offset;
    int level;
    ParseState saved_parse_state = {0};
    TokenString init_str;
    Section *sec;

    size = type_size(type, &align);
    /* If unknown size, we must evaluate it before
       evaluating initializers because
       initializers can generate global data too
       (e.g. string pointers or ISOC99 compound
       literals). It also simplifies local
       initializers handling */
    tok_str_new(&init_str);
    if (size < 0) {
        if (!has_init) 
            error("unknown type size");
        /* get all init string */
        if (has_init == 2) {
            /* only get strings */
            while (tok == TOK_STR || tok == TOK_LSTR) {
                tok_str_add_tok(&init_str);
                next();
            }
        } else {
            level = 0;
            while (level > 0 || (tok != ',' && tok != ';')) {
                if (tok < 0)
                    error("unexpected end of file in initializer");
                tok_str_add_tok(&init_str);
                if (tok == '{')
                    level++;
                else if (tok == '}') {
                    level--;
                    if (level <= 0) {
                        next();
                        break;
                    }
                }
                next();
            }
        }
        tok_str_add(&init_str, -1);
        tok_str_add(&init_str, 0);
        
        /* compute size */
        save_parse_state(&saved_parse_state);

        macro_ptr = init_str.str;
        next();
        decl_initializer(type, NULL, 0, 1, 1);
        /* prepare second initializer parsing */
        macro_ptr = init_str.str;
        next();
        
        /* if still unknown size, error */
        size = type_size(type, &align);
        if (size < 0) 
            error("unknown type size");
    }
    /* take into account specified alignment if bigger */
    if (ad->aligned) {
        if (ad->aligned > align)
            align = ad->aligned;
    } else if (ad->packed) {
        align = 1;
    }
    if ((r & VT_VALMASK) == VT_LOCAL) {
        sec = NULL;
#ifdef CONFIG_TCC_BCHECK
        if (do_bounds_check && (type->t & VT_ARRAY)) 
            loc--;
#endif
        loc = (loc - size) & -align;
        addr = loc;
        /* handles bounds */
        /* XXX: currently, since we do only one pass, we cannot track
           '&' operators, so we add only arrays */
#ifdef CONFIG_TCC_BCHECK
        if (do_bounds_check && (type->t & VT_ARRAY)) {
            unsigned long *bounds_ptr;
            /* add padding between regions */
            loc--;
            /* then add local bound info */
            bounds_ptr = section_ptr_add(lbounds_section, 2 * sizeof(unsigned long));
            bounds_ptr[0] = addr;
            bounds_ptr[1] = size;
        }
#endif
        if (v) {
            /* local variable */
            sym_push(v, type, r, addr);
        } else {
            /* push local reference */
            vset(type, r, addr);
        }
    } else {
        Sym *sym;

        sym = NULL;
        if (v && scope == VT_CONST) {
            /* see if the symbol was already defined */
            sym = sym_find(v);
            if (sym) {
                if (!is_compatible_types(&sym->type, type))
                    error("incompatible types for redefinition of '%s'", 
                          get_tok_str(v, NULL));
                if (sym->type.t & VT_EXTERN) {
                    /* if the variable is extern, it was not allocated */
                    sym->type.t &= ~VT_EXTERN;
                    /* set array size if it was ommited in extern
                       declaration */
                    if ((sym->type.t & VT_ARRAY) && 
                        sym->type.ref->c < 0 &&
                        type->ref->c >= 0)
                        sym->type.ref->c = type->ref->c;
                } else {
                    /* we accept several definitions of the same
                       global variable. this is tricky, because we
                       must play with the SHN_COMMON type of the symbol */
                    /* XXX: should check if the variable was already
                       initialized. It is incorrect to initialized it
                       twice */
                    /* no init data, we won't add more to the symbol */
                    if (!has_init)
                        goto no_alloc;
                }
            }
        }

        /* allocate symbol in corresponding section */
        sec = ad->section;
        if (!sec) {
            if (has_init)
                sec = data_section;
            else if (tcc_state->nocommon)
                sec = bss_section;
        }
        if (sec) {
            data_offset = sec->data_offset;
            data_offset = (data_offset + align - 1) & -align;
            addr = data_offset;
            /* very important to increment global pointer at this time
               because initializers themselves can create new initializers */
            data_offset += size;
            /* add padding if bound check */
#ifdef CONFIG_TCC_BCHECK
            if (do_bounds_check)
                data_offset++;
#endif
            sec->data_offset = data_offset;
            /* allocate section space to put the data */
            if (sec->sh_type != SHT_NOBITS && 
                data_offset > sec->data_allocated)
                section_realloc(sec, data_offset);
            /* align section if needed */
            if (align > sec->sh_addralign)
                sec->sh_addralign = align;
        } else {
            addr = 0; /* avoid warning */
        }

        if (v) {
            if (scope != VT_CONST || !sym) {
                sym = sym_push(v, type, r | VT_SYM, 0);
            }
            /* update symbol definition */
            if (sec) {
                put_extern_sym(sym, sec, addr, size);
            } else {
                ElfW(Sym) *esym;
                /* put a common area */
                put_extern_sym(sym, NULL, align, size);
                /* XXX: find a nicer way */
                esym = &((ElfW(Sym) *)symtab_section->data)[sym->c];
                esym->st_shndx = SHN_COMMON;
            }
        } else {
            CValue cval;

            /* push global reference */
            sym = get_sym_ref(type, sec, addr, size);
            cval.ul = 0;
            vsetc(type, VT_CONST | VT_SYM, &cval);
            vtop->sym = sym;
        }

        /* handles bounds now because the symbol must be defined
           before for the relocation */
#ifdef CONFIG_TCC_BCHECK
        if (do_bounds_check) {
            unsigned long *bounds_ptr;

            greloc(bounds_section, sym, bounds_section->data_offset, R_DATA_32);
            /* then add global bound info */
            bounds_ptr = section_ptr_add(bounds_section, 2 * sizeof(long));
            bounds_ptr[0] = 0; /* relocated */
            bounds_ptr[1] = size;
        }
#endif
    }
    if (has_init) {
        decl_initializer(type, sec, addr, 1, 0);
        /* restore parse state if needed */
        if (init_str.str) {
            tok_str_free(init_str.str);
            restore_parse_state(&saved_parse_state);
        }
    }
 no_alloc: ;
}

void put_func_debug(Sym *sym)
{
    char buf[512];

    /* stabs info */
    /* XXX: we put here a dummy type */
    snprintf(buf, sizeof(buf), "%s:%c1", 
             funcname, sym->type.t & VT_STATIC ? 'f' : 'F');
    put_stabs_r(buf, N_FUN, 0, file->line_num, 0,
                cur_text_section, sym->c);
    /* //gr gdb wants a line at the function */
    put_stabn(N_SLINE, 0, file->line_num, 0); 
    last_ind = 0;
    last_line_num = 0;
}

/* parse an old style function declaration list */
/* XXX: check multiple parameter */
static void func_decl_list(Sym *func_sym)
{
    AttributeDef ad;
    int v;
    Sym *s;
    CType btype, type;

    /* parse each declaration */
    while (tok != '{' && tok != ';' && tok != ',' && tok != TOK_EOF) {
        if (!parse_btype(&btype, &ad)) 
            expect("declaration list");
        if (((btype.t & VT_BTYPE) == VT_ENUM ||
             (btype.t & VT_BTYPE) == VT_STRUCT) && 
            tok == ';') {
            /* we accept no variable after */
        } else {
            for(;;) {
                type = btype;
                type_decl(&type, &ad, &v, TYPE_DIRECT);
                /* find parameter in function parameter list */
                s = func_sym->next;
                while (s != NULL) {
                    if ((s->v & ~SYM_FIELD) == v)
                        goto found;
                    s = s->next;
                }
                error("declaration for parameter '%s' but no such parameter",
                      get_tok_str(v, NULL));
            found:
                /* check that no storage specifier except 'register' was given */
                if (type.t & VT_STORAGE)
                    error("storage class specified for '%s'", get_tok_str(v, NULL)); 
                convert_parameter_type(&type);
                /* we can add the type (NOTE: it could be local to the function) */
                s->type = type;
                /* accept other parameters */
                if (tok == ',')
                    next();
                else
                    break;
            }
        }
        skip(';');
    }
}

/* parse a function defined by symbol 'sym' and generate its code in
   'cur_text_section' */
static void gen_function(Sym *sym)
{
    int saved_nocode_wanted = nocode_wanted;
    nocode_wanted = 0;
    ind = cur_text_section->data_offset;
    /* NOTE: we patch the symbol size later */
    put_extern_sym(sym, cur_text_section, ind, 0);
    funcname = get_tok_str(sym->v, NULL);
    func_ind = ind;
#ifdef CONFIG_TCC_DEBUG
    /* put debug symbol */
    if (do_debug)
        put_func_debug(sym);
#endif
    /* push a dummy symbol to enable local sym storage */
    sym_push2(&local_stack, SYM_FIELD, 0, 0);
    gfunc_prolog(&sym->type);
    rsym = 0;
    block(NULL, NULL, NULL, NULL, 0, 0);
    gsym(rsym);
    gfunc_epilog();
    cur_text_section->data_offset = ind;
    label_pop(&global_label_stack, NULL);
    sym_pop(&local_stack, NULL); /* reset local stack */
    /* end of function */
    /* patch symbol size */
    ((ElfW(Sym) *)symtab_section->data)[sym->c].st_size = 
        ind - func_ind;
#ifdef CONFIG_TCC_DEBUG
    if (do_debug) {
        put_stabn(N_FUN, 0, 0, ind - func_ind);
    }
#endif
    /* It's better to crash than to generate wrong code */
    cur_text_section = NULL;
    funcname = ""; /* for safety */
    func_vt.t = VT_VOID; /* for safety */
    ind = 0; /* for safety */
    nocode_wanted = saved_nocode_wanted;
}

static void gen_inline_functions(void)
{
    Sym *sym;
    CType *type;
    int *str, inline_generated;

    /* iterate while inline function are referenced */
    for(;;) {
        inline_generated = 0;
        for(sym = global_stack; sym != NULL; sym = sym->prev) {
            type = &sym->type;
            if (((type->t & VT_BTYPE) == VT_FUNC) &&
                (type->t & (VT_STATIC | VT_INLINE)) == 
                (VT_STATIC | VT_INLINE) &&
                sym->c != 0) {
                /* the function was used: generate its code and
                   convert it to a normal function */
                str = INLINE_DEF(sym->r);
                sym->r = VT_SYM | VT_CONST;
                sym->type.t &= ~VT_INLINE;

                macro_ptr = str;
                next();
                cur_text_section = text_section;
                gen_function(sym);
                macro_ptr = NULL; /* fail safe */

                tok_str_free(str);
                inline_generated = 1;
            }
        }
        if (!inline_generated)
            break;
    }

    /* free all remaining inline function tokens */
    for(sym = global_stack; sym != NULL; sym = sym->prev) {
        type = &sym->type;
        if (((type->t & VT_BTYPE) == VT_FUNC) &&
            (type->t & (VT_STATIC | VT_INLINE)) == 
            (VT_STATIC | VT_INLINE)) {
            //gr printf("sym %d %s\n", sym->r, get_tok_str(sym->v, NULL));
            if (sym->r == (VT_SYM | VT_CONST)) //gr beware!
                continue;
            str = INLINE_DEF(sym->r);
            tok_str_free(str);
            sym->r = 0; /* fail safe */
        }
    }
}

/* 'l' is VT_LOCAL or VT_CONST to define default storage type */
static void decl(int l)
{
    int v, has_init, r;
    CType type, btype;
    Sym *sym;
    AttributeDef ad;
    
    while (1) {
        if (!parse_btype(&btype, &ad)) {
            /* skip redundant ';' */
            /* XXX: find more elegant solution */
            if (tok == ';') {
                next();
                continue;
            }
            if (l == VT_CONST &&
                (tok == TOK_ASM1 || tok == TOK_ASM2 || tok == TOK_ASM3)) {
                /* global asm block */
                asm_global_instr();
                continue;
            }
            /* special test for old K&R protos without explicit int
               type. Only accepted when defining global data */
            if (l == VT_LOCAL || tok < TOK_DEFINE)
                break;
            btype.t = VT_INT;
        }
        if (((btype.t & VT_BTYPE) == VT_ENUM ||
             (btype.t & VT_BTYPE) == VT_STRUCT) && 
            tok == ';') {
            /* we accept no variable after */
            next();
            continue;
        }
        while (1) { /* iterate thru each declaration */
            type = btype;
            type_decl(&type, &ad, &v, TYPE_DIRECT);
#if 0
            {
                char buf[500];
                type_to_str(buf, sizeof(buf), t, get_tok_str(v, NULL));
                printf("type = '%s'\n", buf);
            }
#endif
            if ((type.t & VT_BTYPE) == VT_FUNC) {
                /* if old style function prototype, we accept a
                   declaration list */
                sym = type.ref;
                if (sym->c == FUNC_OLD)
                    func_decl_list(sym);
            }

            if (tok == '{') {
                if (l == VT_LOCAL)
                    error("cannot use local functions");
                if ((type.t & VT_BTYPE) != VT_FUNC)
                    expect("function definition");

                /* reject abstract declarators in function definition */
                sym = type.ref;
                while ((sym = sym->next) != NULL)
                    if (!(sym->v & ~SYM_FIELD))
                       expect("identifier");
                
                /* XXX: cannot do better now: convert extern line to static inline */
                if ((type.t & (VT_EXTERN | VT_INLINE)) == (VT_EXTERN | VT_INLINE))
                    type.t = (type.t & ~VT_EXTERN) | VT_STATIC;
                
                sym = sym_find(v);
                if (sym) {
                    if ((sym->type.t & VT_BTYPE) != VT_FUNC)
                        goto func_error1;
                    /* specific case: if not func_call defined, we put
                       the one of the prototype */
                    /* XXX: should have default value */
                    r = sym->type.ref->r;
                    if (FUNC_CALL(r) != FUNC_CDECL
                     && FUNC_CALL(type.ref->r) == FUNC_CDECL)
                        FUNC_CALL(type.ref->r) = FUNC_CALL(r);
                    if (FUNC_EXPORT(r))
                        FUNC_EXPORT(type.ref->r) = 1;

                    if (!is_compatible_types(&sym->type, &type)) {
                    func_error1:
                        error("incompatible types for redefinition of '%s'", 
                              get_tok_str(v, NULL));
                    }
                    /* if symbol is already defined, then put complete type */
                    sym->type = type;
                } else {
                    /* put function symbol */
                    sym = global_identifier_push(v, type.t, 0);
                    sym->type.ref = type.ref;
                }

                /* static inline functions are just recorded as a kind
                   of macro. Their code will be emitted at the end of
                   the compilation unit only if they are used */
                if ((type.t & (VT_INLINE | VT_STATIC)) == 
                    (VT_INLINE | VT_STATIC)) {
                    TokenString func_str;
                    int block_level;
                           
                    tok_str_new(&func_str);
                    
                    block_level = 0;
                    for(;;) {
                        int t;
                        if (tok == TOK_EOF)
                            error("unexpected end of file");
                        tok_str_add_tok(&func_str);
                        t = tok;
                        next();
                        if (t == '{') {
                            block_level++;
                        } else if (t == '}') {
                            block_level--;
                            if (block_level == 0)
                                break;
                        }
                    }
                    tok_str_add(&func_str, -1);
                    tok_str_add(&func_str, 0);
                    INLINE_DEF(sym->r) = func_str.str;
                } else {
                    /* compute text section */
                    cur_text_section = ad.section;
                    if (!cur_text_section)
                        cur_text_section = text_section;
                    sym->r = VT_SYM | VT_CONST;
                    gen_function(sym);
                }
                break;
            } else {
                if (btype.t & VT_TYPEDEF) {
                    /* save typedefed type  */
                    /* XXX: test storage specifiers ? */
                    sym = sym_push(v, &type, 0, 0);
                    sym->type.t |= VT_TYPEDEF;
                } else if ((type.t & VT_BTYPE) == VT_FUNC) {
                    /* external function definition */
                    /* specific case for func_call attribute */
                    if (ad.func_attr)
                        type.ref->r = ad.func_attr;
                    external_sym(v, &type, 0);
                } else {
                    /* not lvalue if array */
                    r = 0;
                    if (!(type.t & VT_ARRAY))
                        r |= lvalue_type(type.t);
                    has_init = (tok == '=');
                    if ((btype.t & VT_EXTERN) || 
                        ((type.t & VT_ARRAY) && (type.t & VT_STATIC) &&
                         !has_init && l == VT_CONST && type.ref->c < 0)) {
                        /* external variable */
                        /* NOTE: as GCC, uninitialized global static
                           arrays of null size are considered as
                           extern */
                        external_sym(v, &type, r);
                    } else {
                        type.t |= (btype.t & VT_STATIC); /* Retain "static". */
                        if (type.t & VT_STATIC)
                            r |= VT_CONST;
                        else
                            r |= l;
                        if (has_init)
                            next();
                        decl_initializer_alloc(&type, &ad, r, 
                                               has_init, v, l);
                    }
                }
                if (tok != ',') {
                    skip(';');
                    break;
                }
                next();
            }
        }
    }
}

/* better than nothing, but needs extension to handle '-E' option
   correctly too */
void preprocess_init(TCCState *s1)
{
    s1->include_stack_ptr = s1->include_stack;
    /* XXX: move that before to avoid having to initialize
       file->ifdef_stack_ptr ? */
    s1->ifdef_stack_ptr = s1->ifdef_stack;
    file->ifdef_stack_ptr = s1->ifdef_stack_ptr;

    /* XXX: not ANSI compliant: bound checking says error */
    vtop = vstack - 1;
    s1->pack_stack[0] = 0;
    s1->pack_stack_ptr = s1->pack_stack;
}

/* compile the C file opened in 'file'. Return non zero if errors. */
static int tcc_compile(TCCState *s1)
{
    Sym *define_start;
    char buf[512];
    volatile int section_sym;

#ifdef INC_DEBUG
    printf("%s: **** new file\n", file->filename);
#endif
    preprocess_init(s1);

    cur_text_section = NULL;
    funcname = "";
    anon_sym = SYM_FIRST_ANOM; 

    /* file info: full path + filename */
    section_sym = 0; /* avoid warning */
#ifdef CONFIG_TCC_DEBUG
    if (do_debug) {
        section_sym = put_elf_sym(symtab_section, 0, 0, 
                                  ELFW(ST_INFO)(STB_LOCAL, STT_SECTION), 0, 
                                  text_section->sh_num, NULL);
        getcwd(buf, sizeof(buf));
#ifdef _WIN32
        normalize_slashes(buf);
#endif
        pstrcat(buf, sizeof(buf), "/");
        put_stabs_r(buf, N_SO, 0, 0, 
                    text_section->data_offset, text_section, section_sym);
        put_stabs_r(file->filename, N_SO, 0, 0, 
                    text_section->data_offset, text_section, section_sym);
    }
#endif
    /* an elf symbol of type STT_FILE must be put so that STB_LOCAL
       symbols can be safely used */
    put_elf_sym(symtab_section, 0, 0, 
                ELFW(ST_INFO)(STB_LOCAL, STT_FILE), 0, 
                SHN_ABS, file->filename);

    /* define some often used types */
    int_type.t = VT_INT;

    char_pointer_type.t = VT_BYTE;
    mk_pointer(&char_pointer_type);

    func_old_type.t = VT_FUNC;
    func_old_type.ref = sym_push(SYM_FIELD, &int_type, FUNC_CDECL, FUNC_OLD);

#if defined(TCC_ARM_EABI) && defined(TCC_ARM_VFP)
    float_type.t = VT_FLOAT;
    double_type.t = VT_DOUBLE;

    func_float_type.t = VT_FUNC;
    func_float_type.ref = sym_push(SYM_FIELD, &float_type, FUNC_CDECL, FUNC_OLD);
    func_double_type.t = VT_FUNC;
    func_double_type.ref = sym_push(SYM_FIELD, &double_type, FUNC_CDECL, FUNC_OLD);
#endif

#if 0
    /* define 'void *alloca(unsigned int)' builtin function */
    {
        Sym *s1;

        p = anon_sym++;
        sym = sym_push(p, mk_pointer(VT_VOID), FUNC_CDECL, FUNC_NEW);
        s1 = sym_push(SYM_FIELD, VT_UNSIGNED | VT_INT, 0, 0);
        s1->next = NULL;
        sym->next = s1;
        sym_push(TOK_alloca, VT_FUNC | (p << VT_STRUCT_SHIFT), VT_CONST, 0);
    }
#endif

    define_start = define_stack;
    nocode_wanted = 1;

    if (setjmp(s1->error_jmp_buf) == 0) {
        s1->nb_errors = 0;
        s1->error_set_jmp_enabled = 1;

        ch = file->buf_ptr[0];
        tok_flags = TOK_FLAG_BOL | TOK_FLAG_BOF;
        parse_flags = PARSE_FLAG_PREPROCESS | PARSE_FLAG_TOK_NUM;
        next();
        decl(VT_CONST);
        if (tok != TOK_EOF)
            expect("declaration");

        /* end of translation unit info */
#ifdef CONFIG_TCC_DEBUG
        if (do_debug) {
            put_stabs_r(NULL, N_SO, 0, 0, 
                        text_section->data_offset, text_section, section_sym);
        }
#endif
    }
    s1->error_set_jmp_enabled = 0;

    /* reset define stack, but leave -Dsymbols (may be incorrect if
       they are undefined) */
    free_defines(define_start); 

    gen_inline_functions();

    sym_pop(&global_stack, NULL);
    sym_pop(&local_stack, NULL);

    return s1->nb_errors != 0 ? -1 : 0;
}

/* Preprocess the current file */
static int tcc_preprocess(TCCState *s1)
{
    Sym *define_start;
    BufferedFile *file_ref;
    int token_seen, line_ref;

    preprocess_init(s1);
    define_start = define_stack;
    ch = file->buf_ptr[0];
    tok_flags = TOK_FLAG_BOL | TOK_FLAG_BOF;
    parse_flags = PARSE_FLAG_ASM_COMMENTS | PARSE_FLAG_PREPROCESS |
        PARSE_FLAG_LINEFEED | PARSE_FLAG_SPACES;
    token_seen = 0;
    line_ref = 0;
    file_ref = NULL;

    for (;;) {
        next();
        if (tok == TOK_EOF) {
            break;
        } else if (tok == TOK_LINEFEED) {
            if (!token_seen)
                continue;
            ++line_ref;
            token_seen = 0;
        } else if (!token_seen) {
            int d = file->line_num - line_ref;
            if (file != file_ref || d < 0 || d >= 8)
                fprintf(s1->outfile, "# %d \"%s\"\n", file->line_num, file->filename);
            else
                while (d)
                    fputs("\n", s1->outfile), --d;
            line_ref = (file_ref = file)->line_num;
            token_seen = 1;
        }
        fputs(get_tok_str(tok, &tokc), s1->outfile);
    }
    free_defines(define_start); 
    return 0;
}

#ifdef LIBTCC
int tcc_compile_string(TCCState *s, const char *str)
{
    BufferedFile bf1, *bf = &bf1;
    int ret, len;
    char *buf;

    /* init file structure */
    bf->fd = -1;
    /* XXX: avoid copying */
    len = strlen(str);
    buf = tcc_malloc(len + 1);
    if (!buf)
        return -1;
    memcpy(buf, str, len);
    buf[len] = CH_EOB;
    bf->buf_ptr = buf;
    bf->buf_end = buf + len;
    pstrcpy(bf->filename, sizeof(bf->filename), "<string>");
    bf->line_num = 1;
    file = bf;
    ret = tcc_compile(s);
    file = NULL;
    tcc_free(buf);

    /* currently, no need to close */
    return ret;
}
#endif

/* define a preprocessor symbol. A value can also be provided with the '=' operator */
void tcc_define_symbol(TCCState *s1, const char *sym, const char *value)
{
    BufferedFile bf1, *bf = &bf1;

    pstrcpy(bf->buffer, IO_BUF_SIZE, sym);
    pstrcat(bf->buffer, IO_BUF_SIZE, " ");
    /* default value */
    if (!value) 
        value = "1";
    pstrcat(bf->buffer, IO_BUF_SIZE, value);
    
    /* init file structure */
    bf->fd = -1;
    bf->buf_ptr = bf->buffer;
    bf->buf_end = bf->buffer + strlen(bf->buffer);
    *bf->buf_end = CH_EOB;
    bf->filename[0] = '\0';
    bf->line_num = 1;
    file = bf;
    
    s1->include_stack_ptr = s1->include_stack;

    /* parse with define parser */
    ch = file->buf_ptr[0];
    next_nomacro();
    parse_define();
    file = NULL;
}

/* undefine a preprocessor symbol */
void tcc_undefine_symbol(TCCState *s1, const char *sym)
{
    TokenSym *ts;
    Sym *s;
    ts = tok_alloc(sym, strlen(sym));
    s = define_find(ts->tok);
    /* undefine symbol by putting an invalid name */
    if (s)
        define_undef(s);
}

#ifndef CONFIG_TCC_ASM
void asm_instr(void)
{
    error("inline asm() not supported");
}
void asm_global_instr(void)
{
    error("inline asm() not supported");
}
#endif

#ifdef CONFIG_TCC_BACKTRACE
/* print the position in the source file of PC value 'pc' by reading
   the stabs debug information */
static void rt_printline(unsigned long wanted_pc)
{
    Stab_Sym *sym, *sym_end;
    char func_name[128], last_func_name[128];
    unsigned long func_addr, last_pc, pc;
    const char *incl_files[INCLUDE_STACK_SIZE];
    int incl_index, len, last_line_num, i;
    const char *str, *p;

    fprintf(stderr, "0x%08lx:", wanted_pc);

    func_name[0] = '\0';
    func_addr = 0;
    incl_index = 0;
    last_func_name[0] = '\0';
    last_pc = 0xffffffff;
    last_line_num = 1;
    sym = (Stab_Sym *)stab_section->data + 1;
    sym_end = (Stab_Sym *)(stab_section->data + stab_section->data_offset);
    while (sym < sym_end) {
        switch(sym->n_type) {
            /* function start or end */
        case N_FUN:
            if (sym->n_strx == 0) {
                /* we test if between last line and end of function */
                pc = sym->n_value + func_addr;
                if (wanted_pc >= last_pc && wanted_pc < pc)
                    goto found;
                func_name[0] = '\0';
                func_addr = 0;
            } else {
                str = stabstr_section->data + sym->n_strx;
                p = strchr(str, ':');
                if (!p) {
                    pstrcpy(func_name, sizeof(func_name), str);
                } else {
                    len = p - str;
                    if (len > sizeof(func_name) - 1)
                        len = sizeof(func_name) - 1;
                    memcpy(func_name, str, len);
                    func_name[len] = '\0';
                }
                func_addr = sym->n_value;
            }
            break;
            /* line number info */
        case N_SLINE:
            pc = sym->n_value + func_addr;
            if (wanted_pc >= last_pc && wanted_pc < pc)
                goto found;
            last_pc = pc;
            last_line_num = sym->n_desc;
            /* XXX: slow! */
            strcpy(last_func_name, func_name);
            break;
            /* include files */
        case N_BINCL:
            str = stabstr_section->data + sym->n_strx;
        add_incl:
            if (incl_index < INCLUDE_STACK_SIZE) {
                incl_files[incl_index++] = str;
            }
            break;
        case N_EINCL:
            if (incl_index > 1)
                incl_index--;
            break;
        case N_SO:
            if (sym->n_strx == 0) {
                incl_index = 0; /* end of translation unit */
            } else {
                str = stabstr_section->data + sym->n_strx;
                /* do not add path */
                len = strlen(str);
                if (len > 0 && str[len - 1] != '/')
                    goto add_incl;
            }
            break;
        }
        sym++;
    }

    /* second pass: we try symtab symbols (no line number info) */
    incl_index = 0;
    {
        ElfW(Sym) *sym, *sym_end;
        int type;

        sym_end = (ElfW(Sym) *)(symtab_section->data + symtab_section->data_offset);
        for(sym = (ElfW(Sym) *)symtab_section->data + 1; 
            sym < sym_end;
            sym++) {
            type = ELFW(ST_TYPE)(sym->st_info);
            if (type == STT_FUNC) {
                if (wanted_pc >= sym->st_value &&
                    wanted_pc < sym->st_value + sym->st_size) {
                    pstrcpy(last_func_name, sizeof(last_func_name),
                            strtab_section->data + sym->st_name);
                    goto found;
                }
            }
        }
    }
    /* did not find any info: */
    fprintf(stderr, " ???\n");
    return;
 found:
    if (last_func_name[0] != '\0') {
        fprintf(stderr, " %s()", last_func_name);
    }
    if (incl_index > 0) {
        fprintf(stderr, " (%s:%d", 
                incl_files[incl_index - 1], last_line_num);
        for(i = incl_index - 2; i >= 0; i--)
            fprintf(stderr, ", included from %s", incl_files[i]);
        fprintf(stderr, ")");
    }
    fprintf(stderr, "\n");
}
#endif

#ifdef CONFIG_TCC_DEBUG

#ifdef __i386__
/* fix for glibc 2.1 */
#ifndef REG_EIP
#define REG_EIP EIP
#define REG_EBP EBP
#endif

/* return the PC at frame level 'level'. Return non zero if not found */
static int rt_get_caller_pc(unsigned long *paddr, 
                            ucontext_t *uc, int level)
{
    unsigned long fp;
    int i;

    if (level == 0) {
#if defined(__FreeBSD__)
        *paddr = uc->uc_mcontext.mc_eip;
#elif defined(__dietlibc__)
        *paddr = uc->uc_mcontext.eip;
#else
        *paddr = uc->uc_mcontext.gregs[REG_EIP];
#endif
        return 0;
    } else {
#if defined(__FreeBSD__) 
        fp = uc->uc_mcontext.mc_ebp;
#elif defined(__dietlibc__)
        fp = uc->uc_mcontext.ebp;
#else
        fp = uc->uc_mcontext.gregs[REG_EBP];
#endif
        for(i=1;i<level;i++) {
            /* XXX: check address validity with program info */
            if (fp <= 0x1000 || fp >= 0xc0000000)
                return -1;
            fp = ((unsigned long *)fp)[0];
        }
        *paddr = ((unsigned long *)fp)[1];
        return 0;
    }
}
#elif defined(__x86_64__)
/* return the PC at frame level 'level'. Return non zero if not found */
static int rt_get_caller_pc(unsigned long *paddr,
                            ucontext_t *uc, int level)
{
    unsigned long fp;
    int i;

    if (level == 0) {
        /* XXX: only support linux */
        *paddr = uc->uc_mcontext.gregs[REG_RIP];
        return 0;
    } else {
        fp = uc->uc_mcontext.gregs[REG_RBP];
        for(i=1;i<level;i++) {
            /* XXX: check address validity with program info */
            if (fp <= 0x1000)
                return -1;
            fp = ((unsigned long *)fp)[0];
        }
        *paddr = ((unsigned long *)fp)[1];
        return 0;
    }
}
#else
#warning add arch specific rt_get_caller_pc()
static int rt_get_caller_pc(unsigned long *paddr,
                            ucontext_t *uc, int level)
{
    return -1;
}
#endif /* i386 or x86_64 */
#endif /* CONFIG_TCC_DEBUG */

#ifdef CONFIG_TCC_DEBUG
/* emit a run time error at position 'pc' */
void rt_error(ucontext_t *uc, const char *fmt, ...)
{
    va_list ap;
    unsigned long pc;
    int i;

    va_start(ap, fmt);
    fprintf(stderr, "Runtime error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    for(i=0;i<num_callers;i++) {
        if (rt_get_caller_pc(&pc, uc, i) < 0)
            break;
        if (i == 0)
            fprintf(stderr, "at ");
        else
            fprintf(stderr, "by ");
        rt_printline(pc);
    }
    exit(255);
    va_end(ap);
}
#endif

#ifdef CONFIG_TCC_DEBUG
/* signal handler for fatal errors */
static void sig_error(int signum, siginfo_t *siginf, void *puc)
{
    ucontext_t *uc = puc;

    switch(signum) {
    case SIGFPE:
        switch(siginf->si_code) {
        case FPE_INTDIV:
        case FPE_FLTDIV:
            rt_error(uc, "division by zero");
            break;
        default:
            rt_error(uc, "floating point exception");
            break;
        }
        break;
    case SIGBUS:
    case SIGSEGV:
        if (rt_bound_error_msg && *rt_bound_error_msg)
            rt_error(uc, *rt_bound_error_msg);
        else
            rt_error(uc, "dereferencing invalid pointer");
        break;
    case SIGILL:
        rt_error(uc, "illegal instruction");
        break;
    case SIGABRT:
        rt_error(uc, "abort() called");
        break;
    default:
        rt_error(uc, "caught signal %d", signum);
        break;
    }
    exit(255);
}

#endif

/* copy code into memory passed in by the caller and do all relocations
   (needed before using tcc_get_symbol()).
   returns -1 on error and required size if ptr is NULL */
int tcc_relocate(TCCState *s1, void *ptr)
{
    Section *s;
    unsigned long offset, length, mem;
    int i;

    if (0 == s1->runtime_added) {
        s1->runtime_added = 1;
        s1->nb_errors = 0;
#ifdef TCC_TARGET_PE
        pe_add_runtime(s1);
        relocate_common_syms();
        tcc_add_linker_symbols(s1);
#else
        tcc_add_runtime(s1);
        relocate_common_syms();
        tcc_add_linker_symbols(s1);
        build_got_entries(s1);
#endif
    }

    offset = 0, mem = (unsigned long)ptr;
    for(i = 1; i < s1->nb_sections; i++) {
        s = s1->sections[i];
        if (0 == (s->sh_flags & SHF_ALLOC))
            continue;
        length = s->data_offset;
        s->sh_addr = mem ? (mem + offset + 15) & ~15 : 0;
        offset = (offset + length + 15) & ~15;
    }

    /* relocate symbols */
    relocate_syms(s1, 1);
    if (s1->nb_errors)
        return -1;

#ifdef TCC_TARGET_X86_64
    s1->runtime_plt_and_got_offset = 0;
    s1->runtime_plt_and_got = (char *)(mem + offset);
    /* double the size of the buffer for got and plt entries
       XXX: calculate exact size for them? */
    offset *= 2;
#endif

    if (0 == mem)
        return offset + 15;

    /* relocate each section */
    for(i = 1; i < s1->nb_sections; i++) {
        s = s1->sections[i];
        if (s->reloc)
            relocate_section(s1, s);
    }

    for(i = 1; i < s1->nb_sections; i++) {
        s = s1->sections[i];
        if (0 == (s->sh_flags & SHF_ALLOC))
            continue;
        length = s->data_offset;
        // printf("%-12s %08x %04x\n", s->name, s->sh_addr, length);
        ptr = (void*)s->sh_addr;
        if (NULL == s->data || s->sh_type == SHT_NOBITS)
            memset(ptr, 0, length);
        else
            memcpy(ptr, s->data, length);
        /* mark executable sections as executable in memory */
        if (s->sh_flags & SHF_EXECINSTR)
            set_pages_executable(ptr, length);
    }
#ifdef TCC_TARGET_X86_64
    set_pages_executable(s1->runtime_plt_and_got,
                         s1->runtime_plt_and_got_offset);
#endif
    return 0;
}

/* launch the compiled program with the given arguments */
int tcc_run(TCCState *s1, int argc, char **argv)
{
    int (*prog_main)(int, char **);
    void *ptr;
    int ret;

    ret = tcc_relocate(s1, NULL);
    if (ret < 0)
        return -1;
    ptr = tcc_malloc(ret);
    tcc_relocate(s1, ptr);

    prog_main = tcc_get_symbol_err(s1, "main");
    
#ifdef CONFIG_TCC_DEBUG
    if (do_debug) {
#ifdef CONFIG_TCC_BACKTRACE
        struct sigaction sigact;
        /* install TCC signal handlers to print debug info on fatal
           runtime errors */
        sigact.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigact.sa_sigaction = sig_error;
        sigemptyset(&sigact.sa_mask);
        sigaction(SIGFPE, &sigact, NULL);
        sigaction(SIGILL, &sigact, NULL);
        sigaction(SIGSEGV, &sigact, NULL);
        sigaction(SIGBUS, &sigact, NULL);
        sigaction(SIGABRT, &sigact, NULL);
#else        
        error("debug mode not available");
#endif
    }
#endif

#ifdef CONFIG_TCC_BCHECK
    if (do_bounds_check) {
        void (*bound_init)(void);

        /* set error function */
        rt_bound_error_msg = tcc_get_symbol_err(s1, "__bound_error_msg");

        /* XXX: use .init section so that it also work in binary ? */
        bound_init = (void *)tcc_get_symbol_err(s1, "__bound_init");
        bound_init();
    }
#endif
    ret = (*prog_main)(argc, argv);
    tcc_free(ptr);
    return ret;
}

void tcc_memstats(void)
{
#ifdef MEM_DEBUG
    printf("memory in use: %d\n", mem_cur_size);
#endif
}

static void tcc_cleanup(void)
{
    int i, n;

    if (NULL == tcc_state)
        return;
    tcc_state = NULL;

    /* free -D defines */
    free_defines(NULL);

    /* free tokens */
    n = tok_ident - TOK_IDENT;
    for(i = 0; i < n; i++)
        tcc_free(table_ident[i]);
    tcc_free(table_ident);

    /* free sym_pools */
    dynarray_reset(&sym_pools, &nb_sym_pools);
    /* string buffer */
    cstr_free(&tokcstr);
    /* reset symbol stack */
    sym_free_first = NULL;
    /* cleanup from error/setjmp */
    macro_ptr = NULL;
}

TCCState *tcc_new(void)
{
    const char *p, *r;
    TCCState *s;
    TokenSym *ts;
    int i, c;

    tcc_cleanup();

    s = tcc_mallocz(sizeof(TCCState));
    if (!s)
        return NULL;
    tcc_state = s;
    s->output_type = TCC_OUTPUT_MEMORY;

    /* init isid table */
    for(i=CH_EOF;i<256;i++)
        isidnum_table[i-CH_EOF] = isid(i) || isnum(i);

    /* add all tokens */
    table_ident = NULL;
    memset(hash_ident, 0, TOK_HASH_SIZE * sizeof(TokenSym *));
    
    tok_ident = TOK_IDENT;
    p = tcc_keywords;
    while (*p) {
        r = p;
        for(;;) {
            c = *r++;
            if (c == '\0')
                break;
        }
        ts = tok_alloc(p, r - p - 1);
        p = r;
    }

    /* we add dummy defines for some special macros to speed up tests
       and to have working defined() */
    define_push(TOK___LINE__, MACRO_OBJ, NULL, NULL);
    define_push(TOK___FILE__, MACRO_OBJ, NULL, NULL);
    define_push(TOK___DATE__, MACRO_OBJ, NULL, NULL);
    define_push(TOK___TIME__, MACRO_OBJ, NULL, NULL);

    /* standard defines */
    tcc_define_symbol(s, "__STDC__", NULL);
    tcc_define_symbol(s, "__STDC_VERSION__", "199901L");
#if defined(TCC_TARGET_I386)
    tcc_define_symbol(s, "__i386__", NULL);
#endif
#if defined(TCC_TARGET_X86_64)
    tcc_define_symbol(s, "__x86_64__", NULL);
#endif
#if defined(TCC_TARGET_ARM)
    tcc_define_symbol(s, "__ARM_ARCH_4__", NULL);
    tcc_define_symbol(s, "__arm_elf__", NULL);
    tcc_define_symbol(s, "__arm_elf", NULL);
    tcc_define_symbol(s, "arm_elf", NULL);
    tcc_define_symbol(s, "__arm__", NULL);
    tcc_define_symbol(s, "__arm", NULL);
    tcc_define_symbol(s, "arm", NULL);
    tcc_define_symbol(s, "__APCS_32__", NULL);
#endif
#ifdef TCC_TARGET_PE
    tcc_define_symbol(s, "_WIN32", NULL);
#else
    tcc_define_symbol(s, "__unix__", NULL);
    tcc_define_symbol(s, "__unix", NULL);
#if defined(__linux)
    tcc_define_symbol(s, "__linux__", NULL);
    tcc_define_symbol(s, "__linux", NULL);
#endif
#endif
    /* tiny C specific defines */
    tcc_define_symbol(s, "__TINYC__", NULL);

    /* tiny C & gcc defines */
    tcc_define_symbol(s, "__SIZE_TYPE__", "unsigned int");
    tcc_define_symbol(s, "__PTRDIFF_TYPE__", "int");
#ifdef TCC_TARGET_PE
    tcc_define_symbol(s, "__WCHAR_TYPE__", "unsigned short");
#else
    tcc_define_symbol(s, "__WCHAR_TYPE__", "int");
#endif
    
#ifndef TCC_TARGET_PE
    /* default library paths */
    tcc_add_library_path(s, CONFIG_SYSROOT "/usr/local/lib");
    tcc_add_library_path(s, CONFIG_SYSROOT "/usr/lib");
    tcc_add_library_path(s, CONFIG_SYSROOT "/lib");
#endif

    /* no section zero */
    dynarray_add((void ***)&s->sections, &s->nb_sections, NULL);

    /* create standard sections */
    text_section = new_section(s, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
    data_section = new_section(s, ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
    bss_section = new_section(s, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);

    /* symbols are always generated for linking stage */
    symtab_section = new_symtab(s, ".symtab", SHT_SYMTAB, 0,
                                ".strtab",
                                ".hashtab", SHF_PRIVATE); 
    strtab_section = symtab_section->link;
    
    /* private symbol table for dynamic symbols */
    s->dynsymtab_section = new_symtab(s, ".dynsymtab", SHT_SYMTAB, SHF_PRIVATE,
                                      ".dynstrtab", 
                                      ".dynhashtab", SHF_PRIVATE);
    s->alacarte_link = 1;

#ifdef CHAR_IS_UNSIGNED
    s->char_is_unsigned = 1;
#endif
#if defined(TCC_TARGET_PE) && 0
    /* XXX: currently the PE linker is not ready to support that */
    s->leading_underscore = 1;
#endif
    return s;
}

void tcc_delete(TCCState *s1)
{
    int i;

    tcc_cleanup();

    /* free all sections */
    for(i = 1; i < s1->nb_sections; i++)
        free_section(s1->sections[i]);
    dynarray_reset(&s1->sections, &s1->nb_sections);

    for(i = 0; i < s1->nb_priv_sections; i++)
        free_section(s1->priv_sections[i]);
    dynarray_reset(&s1->priv_sections, &s1->nb_priv_sections);
        
    /* free any loaded DLLs */
    for ( i = 0; i < s1->nb_loaded_dlls; i++) {
        DLLReference *ref = s1->loaded_dlls[i];
        if ( ref->handle )
            dlclose(ref->handle);
    }
    
    /* free loaded dlls array */
    dynarray_reset(&s1->loaded_dlls, &s1->nb_loaded_dlls);

    /* free library paths */
    dynarray_reset(&s1->library_paths, &s1->nb_library_paths);

    /* free include paths */
    dynarray_reset(&s1->cached_includes, &s1->nb_cached_includes);
    dynarray_reset(&s1->include_paths, &s1->nb_include_paths);
    dynarray_reset(&s1->sysinclude_paths, &s1->nb_sysinclude_paths);

    tcc_free(s1);
}

int tcc_add_include_path(TCCState *s1, const char *pathname)
{
    char *pathname1;
    
    pathname1 = tcc_strdup(pathname);
    dynarray_add((void ***)&s1->include_paths, &s1->nb_include_paths, pathname1);
    return 0;
}

int tcc_add_sysinclude_path(TCCState *s1, const char *pathname)
{
    char *pathname1;
    
    pathname1 = tcc_strdup(pathname);
    dynarray_add((void ***)&s1->sysinclude_paths, &s1->nb_sysinclude_paths, pathname1);
    return 0;
}

static int tcc_add_file_internal(TCCState *s1, const char *filename, int flags)
{
    const char *ext;
    ElfW(Ehdr) ehdr;
    int fd, ret;
    BufferedFile *saved_file;

    /* find source file type with extension */
    ext = tcc_fileextension(filename);
    if (ext[0])
        ext++;

    /* open the file */
    saved_file = file;
    file = tcc_open(s1, filename);
    if (!file) {
        if (flags & AFF_PRINT_ERROR) {
            error_noabort("file '%s' not found", filename);
        }
        ret = -1;
        goto fail1;
    }

    if (flags & AFF_PREPROCESS) {
        ret = tcc_preprocess(s1);
    } else if (!ext[0] || !PATHCMP(ext, "c")) {
        /* C file assumed */
        ret = tcc_compile(s1);
    } else 
#ifdef CONFIG_TCC_ASM
    if (!strcmp(ext, "S")) {
        /* preprocessed assembler */
        ret = tcc_assemble(s1, 1);
    } else if (!strcmp(ext, "s")) {
        /* non preprocessed assembler */
        ret = tcc_assemble(s1, 0);
    } else 
#endif
#ifdef TCC_TARGET_PE
    if (!PATHCMP(ext, "def")) {
        ret = pe_load_def_file(s1, file->fd);
    } else
#endif
    {
        fd = file->fd;
        /* assume executable format: auto guess file type */
        ret = read(fd, &ehdr, sizeof(ehdr));
        lseek(fd, 0, SEEK_SET);
        if (ret <= 0) {
            error_noabort("could not read header");
            goto fail;
        } else if (ret != sizeof(ehdr)) {
            goto try_load_script;
        }

        if (ehdr.e_ident[0] == ELFMAG0 &&
            ehdr.e_ident[1] == ELFMAG1 &&
            ehdr.e_ident[2] == ELFMAG2 &&
            ehdr.e_ident[3] == ELFMAG3) {
            file->line_num = 0; /* do not display line number if error */
            if (ehdr.e_type == ET_REL) {
                ret = tcc_load_object_file(s1, fd, 0);
            } else if (ehdr.e_type == ET_DYN) {
                if (s1->output_type == TCC_OUTPUT_MEMORY) {
#ifdef TCC_TARGET_PE
                    ret = -1;
#else
                    void *h;
                    h = dlopen(filename, RTLD_GLOBAL | RTLD_LAZY);
                    if (h)
                        ret = 0;
                    else
                        ret = -1;
#endif
                } else {
                    ret = tcc_load_dll(s1, fd, filename, 
                                       (flags & AFF_REFERENCED_DLL) != 0);
                }
            } else {
                error_noabort("unrecognized ELF file");
                goto fail;
            }
        } else if (memcmp((char *)&ehdr, ARMAG, 8) == 0) {
            file->line_num = 0; /* do not display line number if error */
            ret = tcc_load_archive(s1, fd);
        } else 
#ifdef TCC_TARGET_COFF
        if (*(uint16_t *)(&ehdr) == COFF_C67_MAGIC) {
            ret = tcc_load_coff(s1, fd);
        } else
#endif
#ifdef TCC_TARGET_PE
        if (pe_test_res_file(&ehdr, ret)) {
            ret = pe_load_res_file(s1, fd);
        } else
#endif
        {
            /* as GNU ld, consider it is an ld script if not recognized */
        try_load_script:
            ret = tcc_load_ldscript(s1);
            if (ret < 0) {
                error_noabort("unrecognized file type");
                goto fail;
            }
        }
    }
 the_end:
    tcc_close(file);
 fail1:
    file = saved_file;
    return ret;
 fail:
    ret = -1;
    goto the_end;
}

int tcc_add_file(TCCState *s, const char *filename)
{
    return tcc_add_file_internal(s, filename, AFF_PRINT_ERROR);
}

int tcc_add_library_path(TCCState *s, const char *pathname)
{
    char *pathname1;
    
    pathname1 = tcc_strdup(pathname);
    dynarray_add((void ***)&s->library_paths, &s->nb_library_paths, pathname1);
    return 0;
}

/* find and load a dll. Return non zero if not found */
/* XXX: add '-rpath' option support ? */
int tcc_add_dll(TCCState *s, const char *filename, int flags)
{
    char buf[1024];
    int i;

    for(i = 0; i < s->nb_library_paths; i++) {
        snprintf(buf, sizeof(buf), "%s/%s", 
                 s->library_paths[i], filename);
        if (tcc_add_file_internal(s, buf, flags) == 0)
            return 0;
    }
    return -1;
}

/* the library name is the same as the argument of the '-l' option */
int tcc_add_library(TCCState *s, const char *libraryname)
{
    char buf[1024];
    int i;
    
    /* first we look for the dynamic library if not static linking */
    if (!s->static_link) {
#ifdef TCC_TARGET_PE
        snprintf(buf, sizeof(buf), "%s.def", libraryname);
#else
        snprintf(buf, sizeof(buf), "lib%s.so", libraryname);
#endif
        if (tcc_add_dll(s, buf, 0) == 0)
            return 0;
    }

    /* then we look for the static library */
    for(i = 0; i < s->nb_library_paths; i++) {
        snprintf(buf, sizeof(buf), "%s/lib%s.a", 
                 s->library_paths[i], libraryname);
        if (tcc_add_file_internal(s, buf, 0) == 0)
            return 0;
    }
    return -1;
}

int tcc_add_symbol(TCCState *s, const char *name, void *val)
{
    add_elf_sym(symtab_section, (unsigned long)val, 0, 
                ELFW(ST_INFO)(STB_GLOBAL, STT_NOTYPE), 0,
                SHN_ABS, name);
    return 0;
}

int tcc_set_output_type(TCCState *s, int output_type)
{
    char buf[1024];

    s->output_type = output_type;

    if (!s->nostdinc) {
        /* default include paths */
        /* XXX: reverse order needed if -isystem support */
#ifndef TCC_TARGET_PE
        tcc_add_sysinclude_path(s, CONFIG_SYSROOT "/usr/local/include");
        tcc_add_sysinclude_path(s, CONFIG_SYSROOT "/usr/include");
#endif
        snprintf(buf, sizeof(buf), "%s/include", tcc_lib_path);
        tcc_add_sysinclude_path(s, buf);
#ifdef TCC_TARGET_PE
        snprintf(buf, sizeof(buf), "%s/include/winapi", tcc_lib_path);
        tcc_add_sysinclude_path(s, buf);
#endif
    }

    /* if bound checking, then add corresponding sections */
#ifdef CONFIG_TCC_BCHECK
    if (do_bounds_check) {
        /* define symbol */
        tcc_define_symbol(s, "__BOUNDS_CHECKING_ON", NULL);
        /* create bounds sections */
        bounds_section = new_section(s, ".bounds", 
                                     SHT_PROGBITS, SHF_ALLOC);
        lbounds_section = new_section(s, ".lbounds", 
                                      SHT_PROGBITS, SHF_ALLOC);
    }
#endif

    if (s->char_is_unsigned) {
        tcc_define_symbol(s, "__CHAR_UNSIGNED__", NULL);
    }

#ifdef CONFIG_TCC_DEBUG
    /* add debug sections */
    if (do_debug) {
        /* stab symbols */
        stab_section = new_section(s, ".stab", SHT_PROGBITS, 0);
        stab_section->sh_entsize = sizeof(Stab_Sym);
        stabstr_section = new_section(s, ".stabstr", SHT_STRTAB, 0);
        put_elf_str(stabstr_section, "");
        stab_section->link = stabstr_section;
        /* put first entry */
        put_stabs("", 0, 0, 0, 0);
    }
#endif

    /* add libc crt1/crti objects */
#ifndef TCC_TARGET_PE
    if ((output_type == TCC_OUTPUT_EXE || output_type == TCC_OUTPUT_DLL) &&
        !s->nostdlib) {
        if (output_type != TCC_OUTPUT_DLL)
            tcc_add_file(s, CONFIG_TCC_CRT_PREFIX "/crt1.o");
        tcc_add_file(s, CONFIG_TCC_CRT_PREFIX "/crti.o");
    }
#endif

#ifdef TCC_TARGET_PE
    snprintf(buf, sizeof(buf), "%s/lib", tcc_lib_path);
    tcc_add_library_path(s, buf);
#endif

    return 0;
}

#define WD_ALL    0x0001 /* warning is activated when using -Wall */
#define FD_INVERT 0x0002 /* invert value before storing */

typedef struct FlagDef {
    uint16_t offset;
    uint16_t flags;
    const char *name;
} FlagDef;

static const FlagDef warning_defs[] = {
    { offsetof(TCCState, warn_unsupported), 0, "unsupported" },
    { offsetof(TCCState, warn_write_strings), 0, "write-strings" },
    { offsetof(TCCState, warn_error), 0, "error" },
    { offsetof(TCCState, warn_implicit_function_declaration), WD_ALL,
      "implicit-function-declaration" },
};

static int set_flag(TCCState *s, const FlagDef *flags, int nb_flags,
                    const char *name, int value)
{
    int i;
    const FlagDef *p;
    const char *r;

    r = name;
    if (r[0] == 'n' && r[1] == 'o' && r[2] == '-') {
        r += 3;
        value = !value;
    }
    for(i = 0, p = flags; i < nb_flags; i++, p++) {
        if (!strcmp(r, p->name))
            goto found;
    }
    return -1;
 found:
    if (p->flags & FD_INVERT)
        value = !value;
    *(int *)((uint8_t *)s + p->offset) = value;
    return 0;
}


/* set/reset a warning */
int tcc_set_warning(TCCState *s, const char *warning_name, int value)
{
    int i;
    const FlagDef *p;

    if (!strcmp(warning_name, "all")) {
        for(i = 0, p = warning_defs; i < countof(warning_defs); i++, p++) {
            if (p->flags & WD_ALL)
                *(int *)((uint8_t *)s + p->offset) = 1;
        }
        return 0;
    } else {
        return set_flag(s, warning_defs, countof(warning_defs),
                        warning_name, value);
    }
}

static const FlagDef flag_defs[] = {
    { offsetof(TCCState, char_is_unsigned), 0, "unsigned-char" },
    { offsetof(TCCState, char_is_unsigned), FD_INVERT, "signed-char" },
    { offsetof(TCCState, nocommon), FD_INVERT, "common" },
    { offsetof(TCCState, leading_underscore), 0, "leading-underscore" },
};

/* set/reset a flag */
int tcc_set_flag(TCCState *s, const char *flag_name, int value)
{
    return set_flag(s, flag_defs, countof(flag_defs),
                    flag_name, value);
}

/* set CONFIG_TCCDIR at runtime */
void tcc_set_lib_path(TCCState *s, const char *path)
{
    tcc_lib_path = tcc_strdup(path);
}

#if !defined(LIBTCC)

static int64_t getclock_us(void)
{
#ifdef _WIN32
    struct _timeb tb;
    _ftime(&tb);
    return (tb.time * 1000LL + tb.millitm) * 1000LL;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
#endif
}

void help(void)
{
    printf("tcc version " TCC_VERSION " - Tiny C Compiler - Copyright (C) 2001-2006 Fabrice Bellard\n"
           "usage: tcc [-v] [-c] [-o outfile] [-Bdir] [-bench] [-Idir] [-Dsym[=val]] [-Usym]\n"
           "           [-Wwarn] [-g] [-b] [-bt N] [-Ldir] [-llib] [-shared] [-soname name]\n"
           "           [-static] [infile1 infile2...] [-run infile args...]\n"
           "\n"
           "General options:\n"
           "  -v          display current version, increase verbosity\n"
           "  -c          compile only - generate an object file\n"
           "  -o outfile  set output filename\n"
           "  -Bdir       set tcc internal library path\n"
           "  -bench      output compilation statistics\n"
           "  -run        run compiled source\n"
           "  -fflag      set or reset (with 'no-' prefix) 'flag' (see man page)\n"
           "  -Wwarning   set or reset (with 'no-' prefix) 'warning' (see man page)\n"
           "  -w          disable all warnings\n"
           "Preprocessor options:\n"
           "  -E          preprocess only\n"
           "  -Idir       add include path 'dir'\n"
           "  -Dsym[=val] define 'sym' with value 'val'\n"
           "  -Usym       undefine 'sym'\n"
           "Linker options:\n"
           "  -Ldir       add library path 'dir'\n"
           "  -llib       link with dynamic or static library 'lib'\n"
           "  -shared     generate a shared library\n"
           "  -soname     set name for shared library to be used at runtime\n"
           "  -static     static linking\n"
           "  -rdynamic   export all global symbols to dynamic linker\n"
           "  -r          generate (relocatable) object file\n"
           "Debugger options:\n"
#ifdef CONFIG_TCC_DEBUG
           "  -g          generate runtime debug info\n"
#endif
#ifdef CONFIG_TCC_BCHECK
           "  -b          compile with built-in memory and bounds checker (implies -g)\n"
#endif
#ifdef CONFIG_TCC_BACKTRACE
           "  -bt N       show N callers in stack traces\n"
#endif
           );
}

#define TCC_OPTION_HAS_ARG 0x0001
#define TCC_OPTION_NOSEP   0x0002 /* cannot have space before option and arg */

typedef struct TCCOption {
    const char *name;
    uint16_t index;
    uint16_t flags;
} TCCOption;

enum {
    TCC_OPTION_HELP,
    TCC_OPTION_I,
    TCC_OPTION_D,
    TCC_OPTION_U,
    TCC_OPTION_L,
    TCC_OPTION_B,
    TCC_OPTION_l,
    TCC_OPTION_bench,
    TCC_OPTION_bt,
    TCC_OPTION_b,
    TCC_OPTION_g,
    TCC_OPTION_c,
    TCC_OPTION_static,
    TCC_OPTION_shared,
    TCC_OPTION_soname,
    TCC_OPTION_o,
    TCC_OPTION_r,
    TCC_OPTION_Wl,
    TCC_OPTION_W,
    TCC_OPTION_O,
    TCC_OPTION_m,
    TCC_OPTION_f,
    TCC_OPTION_nostdinc,
    TCC_OPTION_nostdlib,
    TCC_OPTION_print_search_dirs,
    TCC_OPTION_rdynamic,
    TCC_OPTION_run,
    TCC_OPTION_v,
    TCC_OPTION_w,
    TCC_OPTION_pipe,
    TCC_OPTION_E,
};

static const TCCOption tcc_options[] = {
    { "h", TCC_OPTION_HELP, 0 },
    { "?", TCC_OPTION_HELP, 0 },
    { "I", TCC_OPTION_I, TCC_OPTION_HAS_ARG },
    { "D", TCC_OPTION_D, TCC_OPTION_HAS_ARG },
    { "U", TCC_OPTION_U, TCC_OPTION_HAS_ARG },
    { "L", TCC_OPTION_L, TCC_OPTION_HAS_ARG },
    { "B", TCC_OPTION_B, TCC_OPTION_HAS_ARG },
    { "l", TCC_OPTION_l, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP },
    { "bench", TCC_OPTION_bench, 0 },
    { "bt", TCC_OPTION_bt, TCC_OPTION_HAS_ARG },
#ifdef CONFIG_TCC_BCHECK
    { "b", TCC_OPTION_b, 0 },
#endif
    { "g", TCC_OPTION_g, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP },
    { "c", TCC_OPTION_c, 0 },
    { "static", TCC_OPTION_static, 0 },
    { "shared", TCC_OPTION_shared, 0 },
    { "soname", TCC_OPTION_soname, TCC_OPTION_HAS_ARG },
    { "o", TCC_OPTION_o, TCC_OPTION_HAS_ARG },
    { "run", TCC_OPTION_run, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP },
    { "rdynamic", TCC_OPTION_rdynamic, 0 },
    { "r", TCC_OPTION_r, 0 },
    { "Wl,", TCC_OPTION_Wl, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP },
    { "W", TCC_OPTION_W, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP },
    { "O", TCC_OPTION_O, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP },
    { "m", TCC_OPTION_m, TCC_OPTION_HAS_ARG },
    { "f", TCC_OPTION_f, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP },
    { "nostdinc", TCC_OPTION_nostdinc, 0 },
    { "nostdlib", TCC_OPTION_nostdlib, 0 },
    { "print-search-dirs", TCC_OPTION_print_search_dirs, 0 }, 
    { "v", TCC_OPTION_v, TCC_OPTION_HAS_ARG | TCC_OPTION_NOSEP },
    { "w", TCC_OPTION_w, 0 },
    { "pipe", TCC_OPTION_pipe, 0},
    { "E", TCC_OPTION_E, 0},
    { NULL },
};

/* convert 'str' into an array of space separated strings */
static int expand_args(char ***pargv, const char *str)
{
    const char *s1;
    char **argv, *arg;
    int argc, len;

    argc = 0;
    argv = NULL;
    for(;;) {
        while (is_space(*str))
            str++;
        if (*str == '\0')
            break;
        s1 = str;
        while (*str != '\0' && !is_space(*str))
            str++;
        len = str - s1;
        arg = tcc_malloc(len + 1);
        memcpy(arg, s1, len);
        arg[len] = '\0';
        dynarray_add((void ***)&argv, &argc, arg);
    }
    *pargv = argv;
    return argc;
}

static char **files;
static int nb_files, nb_libraries;
static int multiple_files;
static int print_search_dirs;
static int output_type;
static int reloc_output;
static const char *outfile;

int parse_args(TCCState *s, int argc, char **argv)
{
    int optind;
    const TCCOption *popt;
    const char *optarg, *p1, *r1;
    char *r;

    optind = 0;
    while (optind < argc) {

        r = argv[optind++];
        if (r[0] != '-' || r[1] == '\0') {
            /* add a new file */
            dynarray_add((void ***)&files, &nb_files, r);
            if (!multiple_files) {
                optind--;
                /* argv[0] will be this file */
                break;
            }
        } else {
            /* find option in table (match only the first chars */
            popt = tcc_options;
            for(;;) {
                p1 = popt->name;
                if (p1 == NULL)
                    error("invalid option -- '%s'", r);
                r1 = r + 1;
                for(;;) {
                    if (*p1 == '\0')
                        goto option_found;
                    if (*r1 != *p1)
                        break;
                    p1++;
                    r1++;
                }
                popt++;
            }
        option_found:
            if (popt->flags & TCC_OPTION_HAS_ARG) {
                if (*r1 != '\0' || (popt->flags & TCC_OPTION_NOSEP)) {
                    optarg = r1;
                } else {
                    if (optind >= argc)
                        error("argument to '%s' is missing", r);
                    optarg = argv[optind++];
                }
            } else {
                if (*r1 != '\0')
                    return 0;
                optarg = NULL;
            }
            switch(popt->index) {
            case TCC_OPTION_HELP:
                return 0;

            case TCC_OPTION_I:
                if (tcc_add_include_path(s, optarg) < 0)
                    error("too many include paths");
                break;
            case TCC_OPTION_D:
                {
                    char *sym, *value;
                    sym = (char *)optarg;
                    value = strchr(sym, '=');
                    if (value) {
                        *value = '\0';
                        value++;
                    }
                    tcc_define_symbol(s, sym, value);
                }
                break;
            case TCC_OPTION_U:
                tcc_undefine_symbol(s, optarg);
                break;
            case TCC_OPTION_L:
                tcc_add_library_path(s, optarg);
                break;
            case TCC_OPTION_B:
                /* set tcc utilities path (mainly for tcc development) */
                tcc_set_lib_path(s, optarg);
                break;
            case TCC_OPTION_l:
                dynarray_add((void ***)&files, &nb_files, r);
                nb_libraries++;
                break;
            case TCC_OPTION_bench:
                do_bench = 1;
                break;
#ifdef CONFIG_TCC_BACKTRACE
            case TCC_OPTION_bt:
                num_callers = atoi(optarg);
                break;
#endif
#ifdef CONFIG_TCC_BCHECK
            case TCC_OPTION_b:
                do_bounds_check = 1;
                do_debug = 1;
                break;
#endif
#ifdef CONFIG_TCC_DEBUG
            case TCC_OPTION_g:
                do_debug = 1;
                break;
#endif
            case TCC_OPTION_c:
                multiple_files = 1;
                output_type = TCC_OUTPUT_OBJ;
                break;
            case TCC_OPTION_static:
                s->static_link = 1;
                break;
            case TCC_OPTION_shared:
                output_type = TCC_OUTPUT_DLL;
                break;
            case TCC_OPTION_soname:
                s->soname = optarg; 
                break;
            case TCC_OPTION_o:
                multiple_files = 1;
                outfile = optarg;
                break;
            case TCC_OPTION_r:
                /* generate a .o merging several output files */
                reloc_output = 1;
                output_type = TCC_OUTPUT_OBJ;
                break;
            case TCC_OPTION_nostdinc:
                s->nostdinc = 1;
                break;
            case TCC_OPTION_nostdlib:
                s->nostdlib = 1;
                break;
            case TCC_OPTION_print_search_dirs:
                print_search_dirs = 1;
                break;
            case TCC_OPTION_run:
                {
                    int argc1;
                    char **argv1;
                    argc1 = expand_args(&argv1, optarg);
                    if (argc1 > 0) {
                        parse_args(s, argc1, argv1);
                    }
                    multiple_files = 0;
                    output_type = TCC_OUTPUT_MEMORY;
                }
                break;
            case TCC_OPTION_v:
                do {
                    if (0 == verbose++) 
                        printf("tcc version %s\n", TCC_VERSION);
                } while (*optarg++ == 'v');
                break;
            case TCC_OPTION_f:
                if (tcc_set_flag(s, optarg, 1) < 0 && s->warn_unsupported)
                    goto unsupported_option;
                break;
            case TCC_OPTION_W:
                if (tcc_set_warning(s, optarg, 1) < 0 && 
                    s->warn_unsupported)
                    goto unsupported_option;
                break;
            case TCC_OPTION_w:
                s->warn_none = 1;
                break;
            case TCC_OPTION_rdynamic:
                s->rdynamic = 1;
                break;
            case TCC_OPTION_Wl:
                {
                    const char *p;
                    if (strstart(optarg, "-Ttext,", &p)) {
                        s->text_addr = strtoul(p, NULL, 16);
                        s->has_text_addr = 1;
                    } else if (strstart(optarg, "--oformat,", &p)) {
                        if (strstart(p, "elf32-", NULL)) {
                            s->output_format = TCC_OUTPUT_FORMAT_ELF;
                        } else if (!strcmp(p, "binary")) {
                            s->output_format = TCC_OUTPUT_FORMAT_BINARY;
                        } else
#ifdef TCC_TARGET_COFF
                        if (!strcmp(p, "coff")) {
                            s->output_format = TCC_OUTPUT_FORMAT_COFF;
                        } else
#endif
                        {
                            error("target %s not found", p);
                        }
                    } else {
                        error("unsupported linker option '%s'", optarg);
                    }
                }
                break;
            case TCC_OPTION_E:
                output_type = TCC_OUTPUT_PREPROCESS;
                break;
            default:
                if (s->warn_unsupported) {
                unsupported_option:
                    warning("unsupported option '%s'", r);
                }
                break;
            }
        }
    }
    return optind + 1;
}

int main(int argc, char **argv)
{
    int i;
    TCCState *s;
    int nb_objfiles, ret, optind;
    char objfilename[1024];
    int64_t start_time = 0;

    s = tcc_new();
#ifdef _WIN32
    tcc_set_lib_path_w32(s);
#endif
    output_type = TCC_OUTPUT_EXE;
    outfile = NULL;
    multiple_files = 1;
    files = NULL;
    nb_files = 0;
    nb_libraries = 0;
    reloc_output = 0;
    print_search_dirs = 0;
    ret = 0;

    optind = parse_args(s, argc - 1, argv + 1);
    if (print_search_dirs) {
        /* enough for Linux kernel */
        printf("install: %s/\n", tcc_lib_path);
        return 0;
    }
    if (optind == 0 || nb_files == 0) {
        if (optind && verbose)
            return 0;
        help();
        return 1;
    }

    nb_objfiles = nb_files - nb_libraries;

    /* if outfile provided without other options, we output an
       executable */
    if (outfile && output_type == TCC_OUTPUT_MEMORY)
        output_type = TCC_OUTPUT_EXE;

    /* check -c consistency : only single file handled. XXX: checks file type */
    if (output_type == TCC_OUTPUT_OBJ && !reloc_output) {
        /* accepts only a single input file */
        if (nb_objfiles != 1)
            error("cannot specify multiple files with -c");
        if (nb_libraries != 0)
            error("cannot specify libraries with -c");
    }
    

    if (output_type == TCC_OUTPUT_PREPROCESS) {
        if (!outfile) {
            s->outfile = stdout;
        } else {
            s->outfile = fopen(outfile, "w");
            if (!s->outfile)
                error("could not open '%s", outfile);
        }
    } else if (output_type != TCC_OUTPUT_MEMORY) {
        if (!outfile) {
            /* compute default outfile name */
            char *ext;
            const char *name = 
                strcmp(files[0], "-") == 0 ? "a" : tcc_basename(files[0]);
            pstrcpy(objfilename, sizeof(objfilename), name);
            ext = tcc_fileextension(objfilename);
#ifdef TCC_TARGET_PE
            if (output_type == TCC_OUTPUT_DLL)
                strcpy(ext, ".dll");
            else
            if (output_type == TCC_OUTPUT_EXE)
                strcpy(ext, ".exe");
            else
#endif
            if (output_type == TCC_OUTPUT_OBJ && !reloc_output && *ext)
                strcpy(ext, ".o");
            else
                pstrcpy(objfilename, sizeof(objfilename), "a.out");
            outfile = objfilename;
        }
    }

    if (do_bench) {
        start_time = getclock_us();
    }

    tcc_set_output_type(s, output_type);

    /* compile or add each files or library */
    for(i = 0; i < nb_files && ret == 0; i++) {
        const char *filename;

        filename = files[i];
        if (output_type == TCC_OUTPUT_PREPROCESS) {
            if (tcc_add_file_internal(s, filename, 
                    AFF_PRINT_ERROR | AFF_PREPROCESS) < 0)
                ret = 1;
        } else if (filename[0] == '-' && filename[1]) {
            if (tcc_add_library(s, filename + 2) < 0)
                error("cannot find %s", filename);
        } else {
            if (1 == verbose)
                printf("-> %s\n", filename);
            if (tcc_add_file(s, filename) < 0)
                ret = 1;
        }
    }

    /* free all files */
    tcc_free(files);

    if (ret)
        goto the_end;

    if (do_bench) {
        double total_time;
        total_time = (double)(getclock_us() - start_time) / 1000000.0;
        if (total_time < 0.001)
            total_time = 0.001;
        if (total_bytes < 1)
            total_bytes = 1;
        printf("%d idents, %d lines, %d bytes, %0.3f s, %d lines/s, %0.1f MB/s\n", 
               tok_ident - TOK_IDENT, total_lines, total_bytes,
               total_time, (int)(total_lines / total_time), 
               total_bytes / total_time / 1000000.0); 
    }

    if (s->output_type == TCC_OUTPUT_PREPROCESS) {
        if (outfile)
            fclose(s->outfile);
    } else if (s->output_type == TCC_OUTPUT_MEMORY) {
        ret = tcc_run(s, argc - optind, argv + optind);
    } else
        ret = tcc_output_file(s, outfile) ? 1 : 0;
 the_end:
    /* XXX: cannot do it with bound checking because of the malloc hooks */
#ifdef CONFIG_TCC_BCHECK
    if (!do_bounds_check)
        tcc_delete(s);
#endif

#ifdef MEM_DEBUG
    if (do_bench) {
        printf("memory: %d bytes, max = %d bytes\n", mem_cur_size, mem_max_size);
    }
#endif
    return ret;
}

#endif
