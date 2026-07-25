// course/pa12: a function declared in the same scope hides a class
// name (3.3.10p2), but an elaborated-type-specifier still refers to
// the hidden class (3.4.4p2 ignores non-type names). Reduced from
// glibc's <sys/stat.h>, which declares `struct stat` and then
// `extern int stat(const char*, struct stat*)`; the PA39 self-host
// build of x86/elf_program.cpp compiles this shape.
struct stat { int x; };
int stat(const char* p, struct stat* buf) { (*buf).x = 7; return 0; }

int main() {
  struct stat s;
  s.x = 0;
  int r = stat("f", &s);
  return r + s.x - 7;
}
