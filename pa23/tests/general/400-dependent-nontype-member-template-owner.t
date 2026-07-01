// VALIDATION: compile-pass
// N3485 focus: 14.6.2.1 [temp.dep.type], 14.3.2 [temp.arg.nontype],
//              14.5.2 [temp.mem], 14.7.3 [temp.expl.spec]

namespace tuples {
struct nil {};

template<class Head, class Tail>
struct cons {
  typedef Head head_type;
  typedef Tail tail_type;
};

namespace detail {
template<int N>
struct drop_front {
  template<class Tuple>
  struct apply {
    typedef typename drop_front<N - 1>::template apply<Tuple> next;
    typedef typename next::type::tail_type type;
  };
};

template<>
struct drop_front<0> {
  template<class Tuple>
  struct apply {
    typedef Tuple type;
  };
};
}

template<int N, class Tuple>
struct element {
  typedef typename detail::drop_front<N>::template apply<Tuple>::type::head_type type;
};
}

template<class Args>
struct sig {
  typedef typename tuples::element<3, Args>::type type;
};

int main()
{
  typedef tuples::cons<
      int,
      tuples::cons<
          char,
          tuples::cons<short, tuples::cons<long, tuples::nil> > > >
      args;
  typedef sig<args>::type type;
  type value = 1;
  return value == 1 ? 0 : 1;
}
