template<class T, class Alloc>
struct deque_like {
  template<class BiIter, class Sentinel>
  int h(int, BiIter, Sentinel, int);

  template<class BiIter>
  int h(int, BiIter, BiIter, int);
};

template<class Tp, class Allocator>
template<class BiIter, class Sentinel>
int deque_like<Tp, Allocator>::h(int, BiIter, Sentinel, int) {
  return 1;
}

template<class Tp, class Allocator>
template<class BiIter>
int deque_like<Tp, Allocator>::h(int, BiIter, BiIter, int) {
  return 2;
}

int *first();
int *last();

int main() {
  deque_like<int, char> *d = 0;
  return d->h(0, first(), last(), 0) == 2 ? 0 : 1;
}
