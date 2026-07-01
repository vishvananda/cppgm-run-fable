// VALIDATION: compile-pass

namespace n {

template<class C, class Tr, class A>
class basic_string;

template<class C, class Tr, class A>
basic_string<C, Tr, A> operator+(const C*, const basic_string<C, Tr, A>&);

struct tag {};

template<class C>
struct traits {
  static int length(const C*) { return 1; }
};

template<class C>
struct alloc {};

template<class C, class Tr, class A>
class basic_string {
public:
  typedef C value_type;
  typedef A allocator_type;

  struct __alloc_traits {
    static allocator_type select_on_container_copy_construction(const allocator_type& a) {
      return a;
    }
  };

  basic_string() {}

  int size() const { return 1; }
  allocator_type get_allocator() const { return allocator_type(); }

private:
  explicit basic_string(tag, int, const allocator_type&) {}

  friend basic_string operator+<>(const value_type*, const basic_string&);
};

template<class C, class Tr, class A>
basic_string<C, Tr, A> operator+(const C* lhs, const basic_string<C, Tr, A>& rhs) {
  typedef basic_string<C, Tr, A> String;
  int lhs_sz = Tr::length(lhs);
  int rhs_sz = rhs.size();
  String r(tag(), lhs_sz + rhs_sz,
           String::__alloc_traits::select_on_container_copy_construction(
               rhs.get_allocator()));
  return r;
}

}

int main() {
  n::basic_string<char, n::traits<char>, n::alloc<char> > s;
  n::basic_string<char, n::traits<char>, n::alloc<char> > x = "" + s;
  (void)x;
  return 0;
}
