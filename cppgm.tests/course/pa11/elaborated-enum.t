// course/pa11: an elaborated-type-specifier `enum E` followed by a
// declarator refers to the previously declared enumeration (7.1.6.3p3,
// 3.4.4p2); it is not an opaque-enum-declaration. Reduced from glibc's
// <fcntl.h> `struct f_owner_ex { enum __pid_type type; ... };`, which the
// PA39 self-host build of test_runner.cpp compiles.
enum E { A, B };
struct S { enum E e; };
enum E x = B;
int main() {
  S s;
  s.e = A;
  return 0;
}
