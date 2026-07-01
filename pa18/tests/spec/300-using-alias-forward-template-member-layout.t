// VALIDATION: compile-pass
// N3485 focus: 7.1.3 [dcl.typedef], 14.3.1 [temp.arg.type]
// A member object whose type comes from a using-alias to a class-template-id
// must use the later class-template definition for deferred output layout.

template<class T>
class Alloc;

template<class CharT, class AllocT = Alloc<CharT> >
class String;

using IntString = String<int>;

template<class CharT, class AllocT>
class String;

template<class T>
class Alloc {
public:
  typedef T value_type;
  T * p;
};

template<class AllocT, class = typename AllocT::value_type>
struct AllocTraits {
  typedef AllocT allocator_type;
  allocator_type alloc;
};

template<class CharT, class AllocT>
class String {
public:
  typedef AllocTraits<AllocT> traits_type;
  traits_type traits;
  CharT value;
};

struct Holder {
  IntString contents;
};

Holder make_holder()
{
  Holder h;
  return h;
}

int main()
{
  Holder h = make_holder();
  return h.contents.value;
}
