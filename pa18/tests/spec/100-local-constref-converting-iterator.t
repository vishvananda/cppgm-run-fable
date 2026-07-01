// VALIDATION: compile-pass
// N3485 focus: 14.5.1 [temp.class], 14.8 [temp.fct.spec]

struct TreeIter {};

template<class T>
struct map_iter
{
  TreeIter i;
  map_iter() {}
};

template<class T>
struct map_const_iter
{
  TreeIter i;
  map_const_iter() {}
  map_const_iter(map_iter<T> x) : i(x.i) {}
};

template<class T>
void use_const_ref(const map_const_iter<T> &) {}

template<class T>
struct map_like
{
  map_iter<T> end() { return map_iter<T>(); }
};

struct builder
{
  int f()
  {
    struct value_type { bool present; };
    map_like<value_type> m;
    use_const_ref<value_type>(m.end());
    return 0;
  }
};

int main()
{
  builder b;
  return b.f();
}
