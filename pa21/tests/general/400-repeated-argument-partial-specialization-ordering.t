template<int N>
struct iter {};

struct state {};
struct op {};

template<class T>
struct next;

template<int N>
struct next<iter<N> > {
  typedef iter<N + 1> type;
};

template<int N, class First, class Last, class State, class ForwardOp>
struct iter_fold_impl;

template<class First, class Last, class State, class ForwardOp>
struct iter_fold_impl<-1, First, Last, State, ForwardOp>
  : iter_fold_impl<-1, typename next<First>::type, Last, State, ForwardOp> {};

template<class Last, class State, class ForwardOp>
struct iter_fold_impl<-1, Last, Last, State, ForwardOp> {
  static const int value = 7;
};

static_assert(iter_fold_impl<-1, iter<2>, iter<2>, state, op>::value == 7,
              "same iterator terminates");

int main()
{
  return iter_fold_impl<-1, iter<2>, iter<2>, state, op>::value == 7 ? 0 : 1;
}
