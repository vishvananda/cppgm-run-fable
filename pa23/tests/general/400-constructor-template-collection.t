template<class T>
struct X {
  template<class U>
  X(U&&) {}
};

typedef X<int> x_type;

int main() { return 0; }
