/******************************************************/
/* X86 code generator */

/* number of available registers */
#define NB_REGS             4

/* a register can belong to several classes */
#define RC_INT     0x0001 /* generic integer register */
#define RC_FLOAT   0x0002 /* generic float register */
#define RC_IRET    0x0004 /* function returned integer register */
#define RC_FRET    0x0008 /* function returned float register */

/* pretty names for the registers */
enum {
    REG_EAX = 0,
    REG_ECX,
    REG_EDX,
    REG_ST0,
};

/* return registers for function */
#define REG_IRET REG_EAX
#define REG_FRET REG_ST0

/* defined if function parameters must be evaluated in reverse order */
#define INVERT_FUNC_PARAMS

/* defined if structures are passed as pointers. Otherwise structures
   are directly pushed on stack. */
//#define FUNC_STRUCT_PARAM_AS_PTR

/* pointer size, in bytes */
#define PTR_SIZE 4

/* long double size and alignment, in bytes */
#define LDOUBLE_SIZE  12
#define LDOUBLE_ALIGN 4

/* function call context */
typedef struct GFuncContext {
    int args_size;
} GFuncContext;

/******************************************************/
