#include <libtcc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *tccdir = NULL;

typedef int (*callback_type) (void*);

/*
 * Compile source code and call a callback with a pointer to the symbol "f".
 */
static int run_callback(const char *src, callback_type callback) {
  TCCState *s;
  int result;
  void *ptr;
  
  s = tcc_new();
  if (!s)
    return -1;
  if (tccdir)
    tcc_set_lib_path(s, tccdir);
  if (tcc_set_output_type(s, TCC_OUTPUT_MEMORY) == -1)
    return -1;
  if (tcc_compile_string(s, src) == -1)
    return -1;
  if (tcc_relocate(s, TCC_RELOCATE_AUTO) == -1)
    return -1;
  
  ptr = tcc_get_symbol(s, "f");
  if (!ptr)
    return -1;
  result = callback(ptr);
  
  tcc_delete(s);
  
  return result;
}

#define RET_PRIMITIVE_TEST(name, type) \
  static int ret_ ## name ## _test_callback(void *ptr) { \
    type (*callback) (type) = (type(*)(type))ptr; \
    type x = 29; \
    type y = callback(x); \
    return (y == x+x) ? 0 : -1; \
  } \
  \
  static int ret_ ## name ## _test(void) { \
    const char *src = #type " f(" #type " x) {return x+x;}"; \
    return run_callback(src, ret_ ## name ## _test_callback); \
  }

RET_PRIMITIVE_TEST(int, int)
RET_PRIMITIVE_TEST(longlong, long long)
RET_PRIMITIVE_TEST(float, float)
RET_PRIMITIVE_TEST(double, double)
RET_PRIMITIVE_TEST(longdouble, long double)

typedef struct ret_2float_test_type_s {float x, y;} ret_2float_test_type;
typedef ret_2float_test_type (*ret_2float_test_function_type) (ret_2float_test_type);

static int ret_2float_test_callback(void *ptr) {
  ret_2float_test_function_type f = (ret_2float_test_function_type)ptr;
  ret_2float_test_type a = {10, 35};
  ret_2float_test_type r;
  r = f(a);
  return ((r.x == a.x*5) && (r.y == a.y*3)) ? 0 : -1;
}

static int ret_2float_test(void) {
  const char *src =
  "typedef struct ret_2float_test_type_s {float x, y;} ret_2float_test_type;"
  "ret_2float_test_type f(ret_2float_test_type a) {\n"
  "  ret_2float_test_type r = {a.x*5, a.y*3};\n"
  "  return r;\n"
  "}\n";

  return run_callback(src, ret_2float_test_callback);
}

/*
 * reg_pack_test: return a small struct which should be packed into
 * registers (Win32) during return.
 */
typedef struct reg_pack_test_type_s {int x, y;} reg_pack_test_type;
typedef reg_pack_test_type (*reg_pack_test_function_type) (reg_pack_test_type);

static int reg_pack_test_callback(void *ptr) {
  reg_pack_test_function_type f = (reg_pack_test_function_type)ptr;
  reg_pack_test_type a = {10, 35};
  reg_pack_test_type r;
  r = f(a);
  return ((r.x == a.x*5) && (r.y == a.y*3)) ? 0 : -1;
}

static int reg_pack_test(void) {
  const char *src =
  "typedef struct reg_pack_test_type_s {int x, y;} reg_pack_test_type;"
  "reg_pack_test_type f(reg_pack_test_type a) {\n"
  "  reg_pack_test_type r = {a.x*5, a.y*3};\n"
  "  return r;\n"
  "}\n";
  
  return run_callback(src, reg_pack_test_callback);
}

/*
 * sret_test: Create a struct large enough to be returned via sret
 * (hidden pointer as first function argument)
 */
typedef struct sret_test_type_s {long long a, b, c;} sret_test_type;
typedef sret_test_type (*sret_test_function_type) (sret_test_type);

static int sret_test_callback(void *ptr) {
  sret_test_function_type f = (sret_test_function_type)(ptr);
  sret_test_type x = {5436LL, 658277698LL, 43878957LL};
  sret_test_type r = f(x);
  return ((r.a==x.a*35)&&(r.b==x.b*19)&&(r.c==x.c*21)) ? 0 : -1;
}

static int sret_test(void) {
  const char *src =
  "typedef struct sret_test_type_s {long long a, b, c;} sret_test_type;\n"
  "sret_test_type f(sret_test_type x) {\n"
  "  sret_test_type r = {x.a*35, x.b*19, x.c*21};\n"
  "  return r;\n"
  "}\n";
  
  return run_callback(src, sret_test_callback);
}

typedef union one_member_union_test_type_u {int x;} one_member_union_test_type;
typedef one_member_union_test_type (*one_member_union_test_function_type) (one_member_union_test_type);

static int one_member_union_test_callback(void *ptr) {
  one_member_union_test_function_type f = (one_member_union_test_function_type)ptr;
  one_member_union_test_type a, b;
  a.x = 34;
  b = f(a);
  return (b.x == a.x*2) ? 0 : -1;
}

static int one_member_union_test(void) {
  const char *src =
  "typedef union one_member_union_test_type_u {int x;} one_member_union_test_type;\n"
  "one_member_union_test_type f(one_member_union_test_type a) {\n"
  "  one_member_union_test_type b;\n"
  "  b.x = a.x * 2;\n"
  "  return b;\n"
  "}\n";
  return run_callback(src, one_member_union_test_callback);
}

typedef union two_member_union_test_type_u {int x; long y;} two_member_union_test_type;
typedef two_member_union_test_type (*two_member_union_test_function_type) (two_member_union_test_type);

static int two_member_union_test_callback(void *ptr) {
  two_member_union_test_function_type f = (two_member_union_test_function_type)ptr;
  two_member_union_test_type a, b;
  a.x = 34;
  b = f(a);
  return (b.x == a.x*2) ? 0 : -1;
}

static int two_member_union_test(void) {
  const char *src =
  "typedef union two_member_union_test_type_u {int x; long y;} two_member_union_test_type;\n"
  "two_member_union_test_type f(two_member_union_test_type a) {\n"
  "  two_member_union_test_type b;\n"
  "  b.x = a.x * 2;\n"
  "  return b;\n"
  "}\n";
  return run_callback(src, two_member_union_test_callback);
}

#define RUN_TEST(t) \
  if (!testname || (strcmp(#t, testname) == 0)) { \
    fputs(#t "... ", stdout); \
    fflush(stdout); \
    if (t() == 0) { \
      fputs("success\n", stdout); \
    } else { \
      fputs("failure\n", stdout); \
      retval = EXIT_FAILURE; \
    } \
  }

int main(int argc, char **argv) {
  int i;
  const char *testname = NULL;
  int retval = EXIT_SUCCESS;
  
  /* if tcclib.h and libtcc1.a are not installed, where can we find them */
  for (i = 1; i < argc; ++i) {
    if (!memcmp(argv[i], "lib_path=",9))
      tccdir = argv[i] + 9;
    else if (!memcmp(argv[i], "run_test=", 9))
      testname = argv[i] + 9;
  }   

  RUN_TEST(ret_int_test);
  RUN_TEST(ret_longlong_test);
  RUN_TEST(ret_float_test);
  RUN_TEST(ret_double_test);
  RUN_TEST(ret_longdouble_test);
  RUN_TEST(ret_2float_test);
  RUN_TEST(reg_pack_test);
  RUN_TEST(sret_test);
  RUN_TEST(one_member_union_test);
  RUN_TEST(two_member_union_test);
  return retval;
}