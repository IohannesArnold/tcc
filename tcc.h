#define VSTACK_SIZE         64

/* value on stack */
typedef struct SValue {
    int t;
    int c;
} SValue;

/* symbol management */
typedef struct Sym {
    int v;    /* symbol token */
    int t;    /* associated type */
    int c;    /* associated number */
    struct Sym *next; /* next related symbol */
    struct Sym *prev; /* prev symbol in stack */
    struct Sym *hash_next; /* next symbol in hash table */
} Sym; 

/* relocation entry (currently only used for functions or variables */
typedef struct Reloc {
    int type;            /* type of relocation */
    int addr;            /* address of relocation */
    struct Reloc *next;  /* next relocation */
} Reloc;

#define RELOC_ADDR32 1  /* 32 bits relocation */
#define RELOC_REL32  2  /* 32 bits relative relocation */

/* loc : local variable index
   glo : global variable index
   ind : output code ptr
   rsym: return symbol
   prog: output code
   anon_sym: anonymous symbol index
*/
int rsym, anon_sym,
    prog, ind, loc, glo, const_wanted;

SValue vstack[VSTACK_SIZE], *vtop;

/* The current value can be: */
#define VT_VALMASK 0x000f
#define VT_CONST   0x000a  /* constant in vc 
                              (must be first non register value) */
#define VT_LLOCAL  0x000b  /* lvalue, offset on stack */
#define VT_LOCAL   0x000c  /* offset on stack */
#define VT_CMP     0x000d  /* the value is stored in processor flags (in vc) */
#define VT_JMP     0x000e  /* value is the consequence of jmp true */
#define VT_JMPI    0x000f  /* value is the consequence of jmp false */
#define VT_LVAL    0x0010  /* var is an lvalue */
#define VT_LVALN   -17         /* ~VT_LVAL */
#define VT_FORWARD 0x0020  /* value is forward reference 
                              (only used for functions) */
/* storage */
#define VT_EXTERN  0x00000040  /* extern definition */
#define VT_STATIC  0x00000080  /* static variable */
#define VT_TYPEDEF 0x00000100  /* typedef definition */

/* types */
#define VT_STRUCT_SHIFT 16   /* structure/enum name shift (16 bits left) */

#define VT_BTYPE_SHIFT 9
#define VT_INT        (0 << VT_BTYPE_SHIFT)  /* integer type */
#define VT_BYTE       (1 << VT_BTYPE_SHIFT)  /* signed byte type */
#define VT_SHORT      (2 << VT_BTYPE_SHIFT)  /* short type */
#define VT_VOID       (3 << VT_BTYPE_SHIFT)  /* void type */
#define VT_PTR        (4 << VT_BTYPE_SHIFT)  /* pointer increment */
#define VT_ENUM       (5 << VT_BTYPE_SHIFT)  /* enum definition */
#define VT_FUNC       (6 << VT_BTYPE_SHIFT)  /* function type */
#define VT_STRUCT     (7 << VT_BTYPE_SHIFT)  /* struct/union definition */
#define VT_FLOAT      (8 << VT_BTYPE_SHIFT)  /* IEEE float */
#define VT_DOUBLE     (9 << VT_BTYPE_SHIFT)  /* IEEE double */
#define VT_LDOUBLE   (10 << VT_BTYPE_SHIFT)  /* IEEE long double */
#define VT_BOOL      (11 << VT_BTYPE_SHIFT)  /* ISOC99 boolean type */
#define VT_LLONG     (12 << VT_BTYPE_SHIFT)  /* 64 bit integer */
#define VT_LONG      (13 << VT_BTYPE_SHIFT)  /* long integer (NEVER
                                                USED as type, only
                                                during parsing) */
#define VT_BTYPE      (0xf << VT_BTYPE_SHIFT) /* mask for basic type */
#define VT_UNSIGNED   (0x10 << VT_BTYPE_SHIFT)  /* unsigned type */
#define VT_ARRAY      (0x20 << VT_BTYPE_SHIFT)  /* array type (also has VT_PTR) */
#define VT_BITFIELD   (0x40 << VT_BTYPE_SHIFT)  /* bitfield modifier */

#define VT_TYPE    0xfffffe00  /* type mask */

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
#define TOK_ARROW 0xa7 
#define TOK_DOTS  0xa8 /* three dots */
#define TOK_SHR   0xa9 /* unsigned shift right */
#define TOK_UDIV  0xb0 /* unsigned division */
#define TOK_UMOD  0xb1 /* unsigned modulo */
#define TOK_PDIV  0xb2 /* fast division with undefined rounding for pointers */
#define TOK_NUM   0xb3 /* number in tokc */
#define TOK_CCHAR 0xb4 /* char constant in tokc */
#define TOK_STR   0xb5 /* pointer to string in tokc */
#define TOK_TWOSHARPS 0xb6 /* ## preprocessing token */
#define TOK_LCHAR 0xb7
#define TOK_LSTR  0xb8
#define TOK_CFLOAT   0xb9 /* float constant */
#define TOK_CDOUBLE  0xc0 /* double constant */
#define TOK_CLDOUBLE 0xc1 /* long double constant */


 
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
    TOK_AUTO,
    TOK_INLINE,
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
    TOK___LINE__,
    TOK___FILE__,
    TOK___DATE__,
    TOK___TIME__,
    TOK___VA_ARGS__,

    /* special identifiers */
    TOK___FUNC__,
    TOK_MAIN,
};

