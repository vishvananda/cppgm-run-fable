// VALIDATION: compile-pass

typedef unsigned long size_t;

template<class T>
struct range_iterator;

template<class T, size_t N>
struct range_iterator<T[N]>
{
  typedef T * type;
};

template<class T, size_t N>
struct range_iterator<const T[N]>
{
  typedef const T * type;
};

template<class I, class S, class T>
I find_first(I first, S, const T &)
{
  return first;
}

template<class T, size_t N>
T * range_begin(T (&a)[N])
{
  return a;
}

template<class T, size_t N>
const T * range_begin(const T (&a)[N])
{
  return a;
}

template<class R>
typename range_iterator<R>::type begin(R & r)
{
  return range_begin(r);
}

template<class R>
typename range_iterator<const R>::type begin(const R & r)
{
  return range_begin(r);
}

template<class Range, class T>
typename range_iterator<Range>::type find_not(Range & r, const T & x)
{
  return find_first(begin(r), begin(r), x);
}

int main()
{
  int data[5] = {1, 2, 3, 4, 5};
  int * first = find_not(data, 0);
  return first == data ? 0 : 1;
}
