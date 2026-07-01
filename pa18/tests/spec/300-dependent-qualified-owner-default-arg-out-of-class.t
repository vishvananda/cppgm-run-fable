// VALIDATION: compile-pass
// A dependent out-of-class member definition may need to parse the class
// owner while preserving the semantic spelling of a dependent rebound type.

template<class A, class = typename A::type>
struct traits {
  typedef typename traits<A>::template rebind<int> rebound;
  typedef traits<rebound> rebound_traits;
  static int x;
};

template<class A, class B>
int traits<A, B>::x;
