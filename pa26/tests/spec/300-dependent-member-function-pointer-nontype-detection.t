// VALIDATION: compile-pass
// N3485 focus: 14.3.2 [temp.arg.nontype], 14.8.2 [temp.deduct]

typedef char yes_type;

struct no_type
{
  char value[2];
};

template<class U, class Signature>
struct has_select
{
  template<Signature>
  struct helper;

  template<class T>
  static yes_type test(helper<&T::select_on_container_copy_construction> *);

  template<class T>
  static no_type test(...);

  static const bool value = sizeof(test<U>(0)) == sizeof(yes_type);
};

template<class T, bool Copy>
struct allocator
{
  allocator select_on_container_copy_construction() const
  {
    return Copy ? allocator(*this) : allocator();
  }
};

int main()
{
  typedef allocator<char, false> alloc_t;
  return has_select<alloc_t, alloc_t (alloc_t::*)() const>::value ? 0 : 1;
}
