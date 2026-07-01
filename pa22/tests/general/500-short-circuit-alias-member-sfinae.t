template<bool B, class T = int> struct enable_if {};
template<class T> struct enable_if<true, T> { typedef T type; };

template<class A, class B> struct is_same { static const bool value = false; };
template<class A> struct is_same<A, A> { static const bool value = true; };

template<class T> struct remove_const { typedef T type; };
template<class T> struct remove_const<const T> { typedef T type; };
template<class T> using remove_const_t = typename remove_const<T>::type;

template<class T> struct remove_const_ref { typedef typename remove_const<T>::type type; };
template<class T> struct remove_const_ref<T&> { typedef typename remove_const<T>::type type; };
template<class T> struct remove_const_ref<const T&> { typedef T type; };
template<class T> using remove_const_ref_t = typename remove_const_ref<T>::type;

template<class T> struct is_pair { static const bool value = false; };
template<class A, class B> struct pair {
  typedef A first_type;
  typedef B second_type;
};
template<class A, class B> struct is_pair<pair<A, B> > { static const bool value = true; };

struct Key {};

template<class KeyT,
         class Arg,
         typename enable_if<
             is_pair<remove_const_ref_t<Arg> >::value &&
                 is_same<remove_const_t<typename remove_const_ref_t<Arg>::first_type>,
                         KeyT>::value,
             int>::type = 0>
int probe(Arg&&) { return 1; }

template<class KeyT, class Arg>
int probe(Arg&&) { return 0; }

int main() {
  Key k;
  return probe<Key>(k);
}
