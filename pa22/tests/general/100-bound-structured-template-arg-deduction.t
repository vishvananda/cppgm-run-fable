template<class T>
T declval();

template<class _Iter>
struct reverse_iterator {
  _Iter current;
};

template<class _Iter>
_Iter unwrap_iter(reverse_iterator<_Iter> it) {
  return it.current;
}

template<class _Iter, class _Unwrapped = decltype(unwrap_iter(declval<_Iter>()))>
_Unwrapped unwrap_range(_Iter first, _Iter last) {
  return unwrap_iter(first);
}

template<class _InIter, class _Sent>
int dispatch(_InIter first, _Sent last) {
  return *unwrap_range(first, last);
}

int value = 9;

int main() {
  reverse_iterator<int *> first = {&value};
  reverse_iterator<int *> last = {&value};
  return dispatch(first, last) - 9;
}
