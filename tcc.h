#include <setjmp.h>

#ifndef CONFIG_TCC_PREFIX
#define CONFIG_TCC_PREFIX "/usr/local"
#endif

/* path to find crt1.o, crti.o and crtn.o. Only needed when generating
   executables or dlls */
#define CONFIG_TCC_CRT_PREFIX "/usr/lib"

#define INCLUDE_STACK_SIZE  32
#define IFDEF_STACK_SIZE    64
#define VSTACK_SIZE         64
#define STRING_MAX_SIZE     1024

#define TOK_HASH_SIZE       2048 /* must be a power of two */
#define TOK_ALLOC_INCR      512  /* must be a power of two */
#define SYM_HASH_SIZE       1031

/* token symbol management */
typedef struct TokenSym {
    struct TokenSym *hash_next;
    int tok; /* token number */
    int len;
    char str[1];
} TokenSym;

typedef struct CString {
    int size; /* size in bytes */
    void *data; /* either 'char *' or 'int *' */
    int size_allocated;
    void *data_allocated; /* if non NULL, data has been malloced */
} CString;

/* constant value */
typedef union CValue {
    long double ld;
    double d;
    float f;
    int i;
    unsigned int ui;
    unsigned int ul; /* address (should be unsigned long on 64 bit cpu) */
    long long ll;
    unsigned long long ull;
    struct CString *cstr;
    void *ptr;
    int tab[1];
} CValue;

/* value on stack */
typedef struct SValue {
    int t;      /* type */
    unsigned short r;      /* register + flags */
    unsigned short r2;     /* second register, used for 'long long'
                              type. If not used, set to VT_CONST */
    CValue c;              /* constant, if VT_CONST */
    struct Sym *sym;       /* symbol, if (VT_SYM | VT_CONST) */
} SValue;

/* symbol management */
typedef struct Sym {
    int v;    /* symbol token */
    int t;    /* associated type */
    int r;    /* associated register */
    int c;    /* associated number */
    struct Sym *next; /* next related symbol */
    struct Sym *prev; /* prev symbol in stack */
    struct Sym *hash_next; /* next symbol in hash table */
} Sym;

typedef struct SymStack {
  struct Sym *top;
  struct Sym *hash[SYM_HASH_SIZE];
} SymStack;

/* section definition */
/* XXX: use directly ELF structure for parameters ? */
/* special flag to indicate that the section should not be linked to
   the other ones */
#define SHF_PRIVATE 0x80000000

typedef struct Section {
    unsigned long data_offset; /* current data offset */
    unsigned char *data;       /* section data */
    unsigned long data_allocated; /* used for realloc() handling */
    int sh_name;             /* elf section name (only used during output) */
    int sh_num;              /* elf section number */
    int sh_type;             /* elf section type */
    int sh_flags;            /* elf section flags */
    int sh_info;             /* elf section info */
    int sh_addralign;        /* elf section alignment */
    int sh_entsize;          /* elf entry size */
    unsigned long sh_size;   /* section size (only used during output) */
    unsigned long sh_addr;      /* address at which the section is relocated */
    unsigned long sh_offset;      /* address at which the section is relocated */
    int nb_hashed_syms;      /* used to resize the hash table */
    struct Section *link;    /* link to another section */
    struct Section *reloc;   /* corresponding section for relocation, if any */
    struct Section *hash;     /* hash table for symbols */
    struct Section *next;
    char name[64];           /* section name */
} Section;

typedef struct DLLReference {
    int level;
    char name[1];
} DLLReference;

/* GNUC attribute definition */
typedef struct AttributeDef {
    int aligned;
    Section *section;
    unsigned char func_call; /* FUNC_CDECL or FUNC_STDCALL */
} AttributeDef;

#define SYM_STRUCT     0x40000000 /* struct/union/enum symbol space */
#define SYM_FIELD      0x20000000 /* struct/union field symbol space */
#define SYM_FIRST_ANOM (1 << (31 - VT_STRUCT_SHIFT)) /* first anonymous sym */

/* stored in 'Sym.c' field */
#define FUNC_NEW       1 /* ansi function prototype */
#define FUNC_OLD       2 /* old function prototype */
#define FUNC_ELLIPSIS  3 /* ansi function prototype with ... */

/* stored in 'Sym.r' field */
#define FUNC_CDECL     0 /* standard c call */
#define FUNC_STDCALL   1 /* pascal c call */

/* field 'Sym.t' for macros */
#define MACRO_OBJ      0 /* object like macro */
#define MACRO_FUNC     1 /* function like macro */

/* field 'Sym.t' for labels */
#define LABEL_FORWARD  1 /* label is forward defined */

/* type_decl() types */
#define TYPE_ABSTRACT  1 /* type without variable */
#define TYPE_DIRECT    2 /* type with variable */

#define IO_BUF_SIZE 8192

typedef struct BufferedFile {
    unsigned char *buf_ptr;
    unsigned char *buf_end;
    int fd;
    int line_num;    /* current line number - here to simply code */
    int ifndef_macro;  /*'#ifndef macro \n #define macro' search */
    int *ifdef_stack_ptr; /* ifdef_stack value at the start of the file */
    char inc_type;          /* type of include */
    char inc_filename[512]; /* filename specified by the user */
    char filename[1024];    /* current filename - here to simplify code */
    unsigned char buffer[IO_BUF_SIZE + 1]; /* extra size for CH_EOB char */
} BufferedFile;

#define CH_EOB   0       /* end of buffer or '\0' char in file */
#define CH_EOF   (-1)   /* end of file */

/* parsing state (used to save parser state to reparse part of the
   source several times) */
typedef struct ParseState {
    int *macro_ptr;
    int line_num;
    int tok;
    CValue tokc;
} ParseState;

/* used to record tokens */
typedef struct TokenString {
    int *str;
    int len;
    int last_line_num;
} TokenString;

/* include file cache, used to find files faster and also to eliminate
   inclusion if the include file is protected by #ifndef ... #endif */
typedef struct CachedInclude {
    int ifndef_macro;
    char type; /* '"' or '>' to give include type */
    char filename[1]; /* path specified in #include */
} CachedInclude;

/* parser */
struct BufferedFile *file;
int ch, ch1, tok, tok1;
CValue tokc, tok1c;
CString tokcstr; /* current parsed string, if any */
/* if true, line feed is returned as a token. line feed is also
   returned at eof */
int return_linefeed;
/* set to TRUE if eof was reached */
int eof_seen;
/* sections */
Section *text_section, *data_section, *bss_section; /* predefined sections */
Section *cur_text_section; /* current section where function code is
                              generated */
/* bound check related sections */
Section *bounds_section; /* contains global data bound description */
Section *lbounds_section; /* contains local data bound description */
/* symbol sections */
Section *symtab_section, *strtab_section;

/* debug sections */
Section *stab_section, *stabstr_section;

/* loc : local variable index
   ind : output code index
   rsym: return symbol
   prog: output code
   anon_sym: anonymous symbol index
*/
int rsym, anon_sym,
    prog, ind, loc;

/* expression generation modifiers */
int const_wanted; /* true if constant wanted */
int global_expr;  /* true if compound literals must be allocated
                     globally (used during initializers parsing */
int func_vt, func_vc; /* current function return type (used by
                         return instruction) */
int last_line_num, last_ind, func_ind; /* debug last line number and pc */
int tok_ident;
TokenSym **table_ident;
TokenSym *hash_ident[TOK_HASH_SIZE];
char token_buf[STRING_MAX_SIZE + 1];
char *funcname;
SymStack define_stack, global_stack, local_stack, label_stack;

SValue vstack[VSTACK_SIZE], *vtop;
int *macro_ptr, *macro_ptr_allocated;
int char_pointer_type;
int func_old_type;

/* XXX: suppress that ASAP */
static struct TCCState *tcc_state;

/* give the path of the tcc libraries */
static const char *tcc_lib_path = CONFIG_TCC_PREFIX "/lib/tcc";

typedef struct TCCState {
    int output_type;

    BufferedFile **include_stack_ptr;
    int *ifdef_stack_ptr;

    /* include file handling */
    char **include_paths;
    int nb_include_paths;
    char **sysinclude_paths;
    int nb_sysinclude_paths;
    CachedInclude **cached_includes;
    int nb_cached_includes;

    char **library_paths;
    int nb_library_paths;

    /* array of all loaded dlls (including those referenced by loaded
       dlls) */
    DLLReference **loaded_dlls;
    int nb_loaded_dlls;

    /* sections */
    Section **sections;
    int nb_sections; /* number of sections, including first dummy section */

    /* got handling */
    Section *got;
    unsigned long *got_offsets;
    int nb_got_offsets;
    int nb_plt_entries;
    /* give the correspondance from symtab indexes to dynsym indexes */
    int *symtab_to_dynsym;

    /* temporary dynamic symbol sections (for dll loading) */
    Section *dynsymtab_section;
    /* exported dynamic symbol section */
    Section *dynsym;

    /* if true, static linking is performed */
    int static_link;

    /* error handling */
    void *error_opaque;
    void (*error_func)(void *opaque, const char *msg);
    int error_set_jmp_enabled;
    jmp_buf error_jmp_buf;
    int nb_errors;

    /* see include_stack_ptr */
    BufferedFile *include_stack[INCLUDE_STACK_SIZE];

    /* see ifdef_stack_ptr */
    int ifdef_stack[IFDEF_STACK_SIZE];
} TCCState;

/* The current value can be: */
#define VT_VALMASK   0x00ff
#define VT_CONST     0x00f0  /* constant in vc 
                              (must be first non register value) */
#define VT_LLOCAL    0x00f1  /* lvalue, offset on stack */
#define VT_LOCAL     0x00f2  /* offset on stack */
#define VT_CMP       0x00f3  /* the value is stored in processor flags (in vc) */
#define VT_JMP       0x00f4  /* value is the consequence of jmp true (even) */
#define VT_JMPI      0x00f5  /* value is the consequence of jmp false (odd) */
#define VT_LVAL      0x0100  /* var is an lvalue */
#define VT_SYM       0x0200  /* a symbol value is added */
#define VT_MUSTCAST  0x0400  /* value must be casted to be correct (used for
                                char/short stored in integer registers) */
#define VT_MUSTBOUND 0x0800  /* bound checking must be done before
                                dereferencing value */
#define VT_BOUNDED   0x8000  /* value is bounded. The address of the
                                bounding function call point is in vc */
#define VT_LVAL_BYTE     0x1000  /* lvalue is a byte */
#define VT_LVAL_SHORT    0x2000  /* lvalue is a short */
#define VT_LVAL_UNSIGNED 0x4000  /* lvalue is unsigned */
#define VT_LVAL_TYPE     (VT_LVAL_BYTE | VT_LVAL_SHORT | VT_LVAL_UNSIGNED)

/* types */
#define VT_STRUCT_SHIFT 12   /* structure/enum name shift (20 bits left) */

#define VT_INT        0  /* integer type */
#define VT_BYTE       1  /* signed byte type */
#define VT_SHORT      2  /* short type */
#define VT_VOID       3  /* void type */
#define VT_PTR        4  /* pointer */
#define VT_ENUM       5  /* enum definition */
#define VT_FUNC       6  /* function type */
#define VT_STRUCT     7  /* struct/union definition */
#define VT_FLOAT      8  /* IEEE float */
#define VT_DOUBLE     9  /* IEEE double */
#define VT_LDOUBLE   10  /* IEEE long double */
#define VT_BOOL      11  /* ISOC99 boolean type */
#define VT_LLONG     12  /* 64 bit integer */
#define VT_LONG      13  /* long integer (NEVER USED as type, only
                            during parsing) */
#define VT_BTYPE      0x000f /* mask for basic type */
#define VT_UNSIGNED   0x0010  /* unsigned type */
#define VT_ARRAY      0x0020  /* array type (also has VT_PTR) */
#define VT_BITFIELD   0x0040  /* bitfield modifier */

/* storage */
#define VT_EXTERN  0x00000080  /* extern definition */
#define VT_STATIC  0x00000100  /* static variable */
#define VT_TYPEDEF 0x00000200  /* typedef definition */

/* type mask (except storage) */
#define VT_TYPE    (~(VT_EXTERN | VT_STATIC | VT_TYPEDEF))

/* token values */

/* warning: the following compare tokens depend on i386 asm code */
#define TOK_ULT 0x92
#define TOK_UGE 0x93
#define TOK_EQ  0x94
#define TOK_NE  0x95
#define TOK_ULE 0x96
#define TOK_UGT 0x97
#define TOK_LT  0x9c
#define TOK_GE  0x9d
#define TOK_LE  0x9e
#define TOK_GT  0x9f

#define TOK_LAND  0xa0
#define TOK_LOR   0xa1

#define TOK_DEC   0xa2
#define TOK_MID   0xa3 /* inc/dec, to void constant */
#define TOK_INC   0xa4
#define TOK_UDIV  0xb0 /* unsigned division */
#define TOK_UMOD  0xb1 /* unsigned modulo */
#define TOK_PDIV  0xb2 /* fast division with undefined rounding for pointers */
#define TOK_CINT   0xb3 /* number in tokc */
#define TOK_CCHAR 0xb4 /* char constant in tokc */
#define TOK_STR   0xb5 /* pointer to string in tokc */
#define TOK_TWOSHARPS 0xb6 /* ## preprocessing token */
#define TOK_LCHAR    0xb7
#define TOK_LSTR     0xb8
#define TOK_CFLOAT   0xb9 /* float constant */
#define TOK_LINENUM  0xba /* line number info */
#define TOK_CDOUBLE  0xc0 /* double constant */
#define TOK_CLDOUBLE 0xc1 /* long double constant */
#define TOK_UMULL    0xc2 /* unsigned 32x32 -> 64 mul */
#define TOK_ADDC1    0xc3 /* add with carry generation */
#define TOK_ADDC2    0xc4 /* add with carry use */
#define TOK_SUBC1    0xc5 /* add with carry generation */
#define TOK_SUBC2    0xc6 /* add with carry use */
#define TOK_CUINT    0xc8 /* unsigned int constant */
#define TOK_CLLONG   0xc9 /* long long constant */
#define TOK_CULLONG  0xca /* unsigned long long constant */
#define TOK_ARROW    0xcb
#define TOK_DOTS     0xcc /* three dots */
#define TOK_SHR      0xcd /* unsigned shift right */

#define TOK_SHL   0x01 /* shift left */
#define TOK_SAR   0x02 /* signed shift right */
  
/* assignement operators : normal operator or 0x80 */
#define TOK_A_MOD 0xa5
#define TOK_A_AND 0xa6
#define TOK_A_MUL 0xaa
#define TOK_A_ADD 0xab
#define TOK_A_SUB 0xad
#define TOK_A_DIV 0xaf
#define TOK_A_XOR 0xde
#define TOK_A_OR  0xfc
#define TOK_A_SHL 0x81
#define TOK_A_SAR 0x82

/* WARNING: the content of this string encodes token numbers */
static char tok_two_chars[] = "<=\236>=\235!=\225&&\240||\241++\244--\242==\224<<\1>>\2+=\253-=\255*=\252/=\257%=\245&=\246^=\336|=\374->\313..\250##\266";

#define TOK_EOF       (-1)  /* end of file */
#define TOK_LINEFEED  10    /* line feed */

/* all identificators and strings have token above that */
#define TOK_IDENT 256


enum {
    TOK_INT = TOK_IDENT,
    TOK_VOID,
    TOK_CHAR,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_BREAK,
    TOK_RETURN,
    TOK_FOR,
    TOK_EXTERN,
    TOK_STATIC,
    TOK_UNSIGNED,
    TOK_GOTO,
    TOK_DO,
    TOK_CONTINUE,
    TOK_SWITCH,
    TOK_CASE,

    /* ignored types Must have contiguous values */
    TOK_CONST,
    TOK_VOLATILE,
    TOK_LONG,
    TOK_REGISTER,
    TOK_SIGNED,
    TOK___SIGNED__, /* gcc keyword */
    TOK_AUTO,
    TOK_INLINE,
    TOK___INLINE__, /* gcc keyword */
    TOK_RESTRICT,

    /* unsupported type */
    TOK_FLOAT,
    TOK_DOUBLE,
    TOK_BOOL,

    TOK_SHORT,
    TOK_STRUCT,
    TOK_UNION,
    TOK_TYPEDEF,
    TOK_DEFAULT,
    TOK_ENUM,
    TOK_SIZEOF,
    TOK___ATTRIBUTE__,
    
    /* preprocessor only */
    TOK_UIDENT, /* first "user" ident (not keyword) */
    TOK_DEFINE = TOK_UIDENT,
    TOK_INCLUDE,
    TOK_IFDEF,
    TOK_IFNDEF,
    TOK_ELIF,
    TOK_ENDIF,
    TOK_DEFINED,
    TOK_UNDEF,
    TOK_ERROR,
    TOK_LINE,

#define DEF(id, str) id,
#include "tcctok.h"
#undef DEF
};

void cinp(void);
void sum(int l);
void next(void);
void next_nomacro(void);
static int expr_const(void);
void expr_eq(void);
void gexpr(void);
void decl(int l);
int gv(int rc);
void gv2(int rc1, int rc2);
void move_reg(int r, int s);
void save_regs(int n);
void save_reg(int r);
void vpop(void);
void vswap(void);
void vdup(void);
int get_reg(int rc);

int save_reg_forced(int r);
void gen_op(int op);
void force_charshort_cast(int t);
void gen_cast(int t);
void vstore(void);
Sym *sym_find(int v);
Sym *sym_push(int v, int t, int r, int c);

/* type handling */
int type_size(int t, int *a);
int pointed_type(int t);
int pointed_size(int t);
int is_compatible_types(int t1, int t2);
int parse_btype(int *type_ptr, AttributeDef *ad);
int type_decl(AttributeDef *ad, int *v, int t, int td);

void error(const char *fmt, ...);
void rt_error(unsigned long pc, const char *fmt, ...);
void vpushi(int v);
void vset(int t, int r, int v);
void type_to_str(char *buf, int buf_size, 
                 int t, const char *varstr);
char *get_tok_str(int v, CValue *cv);

/* section generation */
void section_realloc(Section *sec, unsigned long new_size);
void *section_ptr_add(Section *sec, unsigned long size);
void greloc(Section *s, Sym *sym, unsigned long addr, int type);
int tcc_add_dll(TCCState *s, const char *filename, int flags);
Sym *external_sym(int v, int u, int r);
Sym *external_global_sym(int v, int u, int r);

#define AFF_PRINT_ERROR     0x0001 /* print error if file not found */
#define AFF_REFERENCED_DLL  0x0002 /* load a referenced dll from another dll */
static int tcc_add_file_internal(TCCState *s, const char *filename, int flags);

void *tcc_malloc(unsigned long size);
void *tcc_mallocz(unsigned long size);

void dynarray_add(void ***ptab, int *nb_ptr, void *data);
 
static inline int isid(int c)
{
    return (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        c == '_';
}

static inline int isnum(int c)
{
    return c >= '0' && c <= '9';
}

/* true if float/double/long double type */
static inline int is_float(int t)
{
    int bt;
    bt = t & VT_BTYPE;
    return bt == VT_LDOUBLE || bt == VT_DOUBLE || bt == VT_FLOAT;
}

/* read one char. MUST call tcc_fillbuf if CH_EOB is read */
#define TCC_GETC(bf) (*(bf)->buf_ptr++)

/* read next char from current input file */
static inline void inp(void)
{
    ch1 = TCC_GETC(file);
    /* end of buffer/file handling */
    if (ch1 == CH_EOB)
        handle_eob();
    if (ch1 == '\n')
        file->line_num++;
    //    printf("ch1=%c 0x%x\n", ch1, ch1);
}


/* memory management */
#ifdef MEM_DEBUG
int mem_cur_size;
int mem_max_size;
#endif

static inline void tcc_free(void *ptr)
{
#ifdef MEM_DEBUG
    mem_cur_size -= malloc_usable_size(ptr);
#endif
    free(ptr);
}
 
static inline void *tcc_realloc(void *ptr, unsigned long size)
{
    void *ptr1;
#ifdef MEM_DEBUG
    mem_cur_size -= malloc_usable_size(ptr);
#endif
    ptr1 = realloc(ptr, size);
#ifdef MEM_DEBUG
    /* NOTE: count not correct if alloc error, but not critical */
    mem_cur_size += malloc_usable_size(ptr1);
    if (mem_cur_size > mem_max_size)
        mem_max_size = mem_cur_size;
#endif
    return ptr1;
}

