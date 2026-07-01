// VALIDATION: compile-pass
// A concrete instantiation must resolve auto locals through dependent helper
// defaults before output/lowering.

template<class T>
T && move_like(T & value)
{
  return static_cast<T &&>(value);
}

template<class T>
T declval();

template<class T, bool = true>
struct unwrap_impl_like {
  static T unwrap(T value)
  {
    return value;
  }
};

template<class T, class Impl = unwrap_impl_like<T>, int = 0>
decltype(Impl::unwrap(declval<T>())) unwrap_iter_like(T value)
{
  return Impl::unwrap(value);
}

template<class T1, class T2>
struct pair_like {
  T1 first;
  T2 second;

  pair_like(pair_like &&) = default;

  template<class U1, class U2>
  pair_like(U1 && u1, U2 && u2)
    : first(move_like(u1)), second(move_like(u2))
  {
  }
};

template<class T1, class T2>
pair_like<T1, T2> make_pair_like(T1 && first, T2 && second)
{
  return pair_like<T1, T2>(move_like(first), move_like(second));
}

template<class Iter,
         class Unwrapped = decltype(unwrap_iter_like(declval<Iter>()))>
pair_like<Unwrapped, Unwrapped> unwrap_range_like(Iter first, Iter last)
{
  return make_pair_like<Unwrapped, Unwrapped>(unwrap_iter_like(move_like(first)),
                                              unwrap_iter_like(move_like(last)));
}

struct copy_algorithm {
  template<class InIter, class OutIter>
  pair_like<InIter, OutIter> operator()(InIter first, InIter, OutIter result)
  {
    return make_pair_like<InIter, OutIter>(move_like(first), move_like(result));
  }
};

template<class Algorithm,
         class InIter,
         class Sent,
         class OutIter>
pair_like<InIter, OutIter> copy_move_unwrap_like(InIter first,
                                                 Sent last,
                                                 OutIter out_first)
{
  auto range = unwrap_range_like(first, move_like(last));
  auto result = Algorithm()(move_like(range.first),
                            move_like(range.second),
                            unwrap_iter_like(out_first));
  return make_pair_like<InIter, OutIter>(move_like(result.first),
                                         move_like(result.second));
}

int main()
{
  int value = 1;
  int * ptr = &value;
  pair_like<int *, int *> result =
      copy_move_unwrap_like<copy_algorithm>(ptr, ptr, ptr);
  return result.first == ptr && result.second == ptr ? 0 : 1;
}
