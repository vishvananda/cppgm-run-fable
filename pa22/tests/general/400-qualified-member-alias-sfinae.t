// VALIDATION: compile-pass
// Substitution through a qualified member alias may participate in a member
// template SFINAE guard.

template<bool B, class T>
struct enable_if_impl {};

template<class T>
struct enable_if_impl<true, T> {
  typedef T type;
};

template<class A, class B>
struct same {
  enum { value = 0 };
};

template<class A>
struct same<A, A> {
  enum { value = 1 };
};

template<class T>
struct traits {
  typedef T& reference;
};

template<class T>
struct box {
  typedef T value_type;
  typedef T& reference;

  template<class It,
           typename enable_if_impl<
               same<typename traits<It>::reference, reference>::value &&
               same<value_type, int>::value,
               int>::type = 0>
  char insert(It, It) {
    return char(7);
  }
};

int main()
{
  box<int> b;
  return int(b.insert(1, 1));
}
