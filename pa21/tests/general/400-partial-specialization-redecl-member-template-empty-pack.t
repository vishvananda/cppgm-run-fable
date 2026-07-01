template<bool B>
struct bool_constant { static const bool value = B; };

template<class A, class B>
struct is_same : bool_constant<false> {};

template<class A>
struct is_same<A, A> : bool_constant<true> {};

struct outer9 {};

struct outer0 { typedef outer9 rebound; };

template<class... InnerAllocs>
class scoped;

template<class OuterAlloc, class... InnerAllocs>
class scoped<OuterAlloc, InnerAllocs...>;

template<class... InnerAllocs>
class scoped;

template<class OuterAlloc, class... InnerAllocs>
class scoped<OuterAlloc, InnerAllocs...>;

template<class OuterAlloc, class... InnerAllocs>
class scoped<OuterAlloc, InnerAllocs...> {
public:
  template<class U>
  struct rebind {
    typedef scoped<typename OuterAlloc::rebound, InnerAllocs...> other;
  };
};

typedef scoped<outer0> scoped0;
typedef scoped<outer9> rebound0;

static_assert(is_same<scoped0::rebind<int>::other, rebound0>::value,
              "empty selected partial pack should stay empty");

int main() { return 0; }
