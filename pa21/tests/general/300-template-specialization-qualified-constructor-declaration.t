template<class T>
struct S {
  S();
};

template<>
S<int>::S();

int main() {
  return 0;
}
