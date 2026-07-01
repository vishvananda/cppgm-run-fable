int converted;

struct Holder {
  int *p;

  explicit Holder(int *q) : p(q) {}

  Holder(Holder &&other) : p(other.p) {
    other.p = 0;
  }

  template <class U>
  operator U() {
    converted = 1;
    return U(p);
  }
};

template <class T>
T &&forward_like(T &value) {
  return static_cast<T &&>(value);
}

int seen;

void consume(Holder value) {
  seen = value.p ? *value.p : -1;
}

int main() {
  int x = 9;
  Holder h(&x);
  consume(forward_like<Holder>(h));
  return seen == 9 && converted == 0 ? 0 : 1;
}
