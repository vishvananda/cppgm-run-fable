template<long N>
struct long_ {
  static const long value = N;
};

template<class T>
struct type_wrapper {
  typedef T type;
};

template<class T>
struct wrapped_type;

template<class T>
struct wrapped_type<type_wrapper<T> > {
  typedef T type;
};

template<class A, class B>
struct is_same {
  static const bool value = false;
};

template<class A>
struct is_same<A, A> {
  static const bool value = true;
};

struct vector0 {
  typedef long_<0> lower_bound_;
  typedef lower_bound_ upper_bound_;
  static type_wrapper<void> item_(...);
};

template<class T, class Base>
struct v_item : Base {
  typedef typename Base::upper_bound_ index_;
  typedef long_<index_::value + 1> upper_bound_;
  static type_wrapper<T> item_(index_);
  using Base::item_;
};

template<class T0>
struct vector1 : v_item<T0, vector0> {
};

template<class T0, class T1>
struct vector2 : v_item<T1, vector1<T0> > {
};

template<class T0, class T1, class T2>
struct vector3 : v_item<T2, vector2<T0, T1> > {
};

template<class T0, class T1, class T2, class T3>
struct vector4 : v_item<T3, vector3<T0, T1, T2> > {
};

template<class Vector, long n_>
struct v_at_impl {
  typedef long_<(Vector::lower_bound_::value + n_)> index_;
  typedef __typeof__(Vector::item_(index_())) type;
};

template<class Vector, long n_>
struct v_at : wrapped_type<typename v_at_impl<Vector, n_>::type> {
};

class C;
class D;
class E;
class F;
typedef typename v_at<vector4<C, D, E, F>, 0>::type first_type;
typedef typename v_at<vector4<C, D, E, F>, 3>::type last_type;
static_assert(is_same<first_type, C>::value, "first element should not use the ellipsis fallback");
static_assert(is_same<last_type, F>::value, "last element should still resolve directly");

int main()
{
  first_type *p = 0;
  last_type *q = 0;
  (void)q;
  return p != 0;
}
