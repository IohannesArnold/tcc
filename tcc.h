#define VSTACK_SIZE         64

/* token symbol management */
typedef struct TokenSym {
    struct TokenSym *hash_next;
    int tok; /* token number */
    int len;
    char str[1];
} TokenSym;

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
    struct TokenSym *ts;
    struct Sym *sym;
    int tab[1];
} CValue;

/* value on stack */
typedef struct SValue {
    int t;      /* type */
    unsigned short r;      /* register + flags */
    unsigned short r2;     /* second register, used for 'long long'
                              type. If not used, set to VT_CONST */
    CValue c;   /* constant */
} SValue;

/* symbol management */
typedef struct Sym {
    int v;    /* symbol token */
    int t;    /* associated type */
    int c;    /* associated number */
    int r;    /* associated register */
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

#define SYM_STRUCT     0x40000000 /* struct/union/enum symbol space */
#define SYM_FIELD      0x20000000 /* struct/union field symbol space */
#define SYM_FIRST_ANOM (1 << (31 - VT_STRUCT_SHIFT)) /* first anonymous sym */


/* loc : local variable index
   ind : output code index
   rsym: return symbol
   prog: output code
   anon_sym: anonymous symbol index
*/
int rsym, anon_sym,
    prog, ind, loc, const_wanted;

int func_vt, func_vc; /* current function return type (used by
                         return instruction) */

SValue vstack[VSTACK_SIZE], *vtop;

/* The current value can be: */
#define VT_VALMASK  0x00ff
#define VT_CONST    0x00f0  /* constant in vc 
                              (must be first non register value) */
#define VT_LLOCAL   0x00f1  /* lvalue, offset on stack */
#define VT_LOCAL    0x00f2  /* offset on stack */
#define VT_CMP      0x00f3  /* the value is stored in processor flags (in vc) */
#define VT_JMP      0x00f4  /* value is the consequence of jmp true (even) */
#define VT_JMPI     0x00f5  /* value is the consequence of jmp false (odd) */
#define VT_LVAL     0x0100  /* var is an lvalue */
#define VT_FORWARD  0x0200  /* value is forward reference */
#define VT_MUSTCAST 0x0400  /* value must be casted to be correct (used for
                               char/short stored in integer registers) */

/* types */
#define VT_STRUCT_SHIFT 16   /* structure/enum name shift (16 bits left) */

#define VT_INT        0  /* integer type */
#define VT_BYTE       1  /* signed byte type */
#define VT_SHORT      2  /* short type */
#define VT_VOID       3  /* void type */
#define VT_PTR        4  /* pointer increment */
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
#define TOK_LCHAR 0xb7
#define TOK_LSTR  0xb8
#define TOK_CFLOAT   0xb9 /* float constant */
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
    TOK___LINE__,
    TOK___FILE__,
    TOK___DATE__,
    TOK___TIME__,
    TOK___VA_ARGS__,

    /* special identifiers */
    TOK___FUNC__,
    TOK_MAIN,
    /* attribute identifiers */
    TOK_SECTION,
    TOK___SECTION__,
    TOK_ALIGNED,
    TOK___ALIGNED__,
    TOK_UNUSED,
    TOK___UNUSED__,
};

/* true if float/double/long double type */
static inline int is_float(int t)
{
    int bt;
    bt = t & VT_BTYPE;
    return bt == VT_LDOUBLE || bt == VT_DOUBLE || bt == VT_FLOAT;
}

