template<bool B, class T = int>
struct enable_if {};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<class Alloc, class P>
struct has_destroy
{
  static const bool value = false;
};

template<class T>
struct alloc
{
};

template<class T>
struct has_destroy<alloc<T>, T*>
{
  static const bool value = true;
};

template<class Alloc>
struct traits
{
  typedef Alloc allocator_type;

  template<class T,
           typename enable_if<has_destroy<allocator_type, T*>::value, int>::type = 0>
  static int destroy(Alloc&, T*)
  {
    return 1;
  }

  template<class T,
           typename enable_if<!has_destroy<allocator_type, T*>::value, int>::type = 0>
  static int destroy(Alloc&, T*)
  {
    return 2;
  }
};

int main()
{
  int value = 0;
  alloc<int> a;
  return traits<alloc<int> >::destroy(a, &value) - 1;
}
