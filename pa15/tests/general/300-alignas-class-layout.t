struct alignas(16) ForwardAligned;
struct alignas(16) ForwardAligned { char value; };
struct alignas(32) DeclAligned { char value; };
struct alignas(long double) TypeAligned { char value; };

int main() {
  if(alignof(ForwardAligned) != 16 || sizeof(ForwardAligned) != 16) {
    return 1;
  }
  if(alignof(DeclAligned) != 32 || sizeof(DeclAligned) != 32) {
    return 2;
  }
  if(alignof(TypeAligned) != alignof(long double) ||
     sizeof(TypeAligned) != alignof(long double)) {
    return 3;
  }
  return 0;
}
