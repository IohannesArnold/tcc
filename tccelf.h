#define ARMAG  "!<arch>\012"	/* For COFF and a.out archives */

/* put stab debug information */

typedef struct {
    unsigned int n_strx;         /* index into string table of name */
    unsigned char n_type;        /* type of symbol */
    unsigned char n_other;       /* misc info (usually empty) */
    unsigned short n_desc;       /* description field */
    unsigned int n_value;        /* value of symbol */
} Stab_Sym;

int add_elf_sym(Section *s, unsigned long value, unsigned long size,
                       int info, int other, int sh_num, const char *name);
int put_elf_sym(Section *s, 
                       unsigned long value, unsigned long size,
                       int info, int other, int shndx, const char *name);
void put_elf_reloc(Section *symtab, Section *s, unsigned long offset,
                          int type, int symbol);
void tcc_add_runtime(TCCState *s1);
void put_stabs_r(const char *str, int type, int other, int desc, 
                 unsigned long value, Section *sec, int sym_index);
void relocate_common_syms(void);
void relocate_syms(TCCState *s1, int do_resolve);
void relocate_section(TCCState *s1, Section *s);
void *tcc_get_symbol(TCCState *s, const char *name);
Section *new_symtab(TCCState *s1,
                    const char *symtab_name, int sh_type, int sh_flags,
                    const char *strtab_name, 
                    const char *hash_name, int hash_sh_flags);
int tcc_load_object_file(TCCState *s1, 
                         int fd, unsigned long file_offset);
int tcc_load_dll(TCCState *s1, int fd, const char *filename, int level);
int tcc_load_archive(TCCState *s1, int fd);
int tcc_load_ldscript(TCCState *s1);
