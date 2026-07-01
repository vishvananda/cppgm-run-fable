template<class...>
using void_t = void;

struct nat {};

template<class Default, class Void, template<class...> class Op, class... Args>
struct detector {
  typedef Default type;
};

template<class Default, template<class...> class Op, class... Args>
struct detector<Default, void_t<Op<Args...> >, Op, Args...> {
  typedef Op<Args...> type;
};

template<class Default, template<class...> class Op, class... Args>
using detected_or_t = typename detector<Default, void, Op, Args...>::type;

template<class T>
struct iterator_traits {
  typedef int iterator_category;
};

template<class T>
using iter_cat = typename T::iterator_category;

template<class T, class U>
struct conv {
  typedef detected_or_t<nat, iter_cat, iterator_traits<T> > type;
};

conv<int, int>::type x;

int main() {
  return 0;
}
