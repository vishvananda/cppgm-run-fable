// VALIDATION: compile-pass

template<class C, bool B>
struct Iter;

struct Copy {
  template<class C, bool B>
  int operator()(Iter<C, B> first, Iter<C, B> last, Iter<C, false> result) const;
};

template<class C, bool B>
struct Iter {
  Iter(int v) : bit(v) {}

private:
  int bit;

  template<class D, bool E>
  friend int Copy::operator()(Iter<D, E>, Iter<D, E>, Iter<D, false>) const;
};

template<class C, bool B>
int Copy::operator()(Iter<C, B> first, Iter<C, B>, Iter<C, false> result) const {
  return first.bit == result.bit ? 0 : 1;
}

int main() {
  Iter<int, false> it(7);
  Copy copy;
  return copy(it, it, it);
}
