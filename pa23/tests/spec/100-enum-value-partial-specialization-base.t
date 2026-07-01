// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]

template<class T, unsigned Id>
struct Alloc
{
  char value;
};

enum Construction
{
  Prefix,
  Other
};

template<Construction Kind, unsigned Tag>
struct Base;

template<unsigned Tag>
struct Base<Prefix, Tag>
{
  typedef Alloc<int, Tag> allocator_type;
};

template<Construction Kind, unsigned Tag>
struct Holder : Base<Kind, Tag>
{
};

template<class T>
struct allocator_type_of
{
  typedef typename T::allocator_type type;
};

template<class T>
struct size_of_type
{
  static const int value = sizeof(T);
};

static_assert(size_of_type<typename allocator_type_of<Holder<Prefix, 0> >::type>::value ==
                  sizeof(Alloc<int, 0>),
              "ok");

int main()
{
  return 0;
}
