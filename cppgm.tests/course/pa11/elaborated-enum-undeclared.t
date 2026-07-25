// course/pa11: an elaborated-type-specifier `enum E` cannot declare a new
// enumeration; referring to an undeclared name is an error (7.1.6.3p3).
struct S { enum E e; };
int main() { return 0; }
