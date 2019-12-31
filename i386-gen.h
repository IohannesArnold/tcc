
/******************************************************/
/* X86 code generator */


/* number of available registers */
#define NB_REGS             4

#define NB_REG_CLASSES      2

/* a register can belong to several classes */
#define REG_CLASS_INT    0x0001
#define REG_CLASS_FLOAT  0x0002

/* pretty names for the registers */
enum {
    REG_EAX = 0,
    REG_ECX,
    REG_EDX,
    REG_ST0,
};

/* integer return register for functions */
#define FUNC_RET_REG        0 
/* float return register for functions */
#define FUNC_RET_FREG       3

/* defined if function parameters must be evaluated in reverse order */
#define INVERT_FUNC_PARAMS

/* defined if structures are passed as pointers. Otherwise structures
   are directly pushed on stack. */
//#define FUNC_STRUCT_PARAM_AS_PTR

/* function call context */
typedef struct GFuncContext {
    int args_size;
} GFuncContext;

/******************************************************/
