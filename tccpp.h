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

#include "stddef.h"

int handle_eob(void);
void inp(void);
void minp(void);
uint8_t *parse_comment(uint8_t *p);
int is_space(int ch);
void preprocess_skip(void);
void restore_parse_state(ParseState *s);
int tok_ext_size(int t);
void tok_str_new(TokenString *s);
void tok_str_free(int *str);
void tok_str_add(TokenString *s, int t);
void tok_str_add_tok(TokenString *s);
void define_push(int v, int macro_type, int *str, Sym *first_arg);
void define_undef(Sym *s);
Sym *define_find(int v);
void free_defines(Sym *b);
Sym *label_find(int v);
Sym *label_push(Sym **ptop, int v, int flags);
void label_pop(Sym **ptop, Sym *slast);
void parse_define(void);
void bn_lshift(unsigned int *bn, int shift, int or_val);
void bn_zero(unsigned int *bn);
void parse_number(const char *p);
void next_nomacro_spc(void);
void next_nomacro(void);
void next(void);
void unget_tok(int last_tok);
