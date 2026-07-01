template<class T>
struct holder {
  T* ptr;

  template<class U>
  explicit holder(U* p) : ptr(p) {}

  template<class U>
  static holder make(U* p) {
    return holder(p);
  }
};

int *source();

int main() {
  return holder<int>::make(source()).ptr ? 1 : 0;
}
