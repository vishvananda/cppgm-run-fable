// VALIDATION: compile-pass
// N3485 focus: 12.2 [class.temporary], 13.3.3.1.4 [over.ics.ref]

struct Iterator
{
  static int destroyed;

  ~Iterator() noexcept { ++destroyed; }
};

int Iterator::destroyed = 0;

template<class It, class Alloc>
struct AssignRange
{
  It first;
  It last;
  Alloc & alloc;

  AssignRange(const It & f, const It & l, Alloc & a) noexcept
    : first(f), last(l), alloc(a)
  {
  }

  void operator()(int *) const noexcept
  {
  }
};

template<class It, class Alloc>
AssignRange<It, Alloc> make_assign_range(const It & first,
                                         const It & last,
                                         Alloc & alloc) noexcept
{
  return AssignRange<It, Alloc>(first, last, alloc);
}

struct Buffer
{
  int alloc;

  Buffer() noexcept : alloc(0)
  {
  }

  Iterator begin() noexcept
  {
    return Iterator();
  }

  Iterator end() noexcept
  {
    return Iterator();
  }

  template<class Functor>
  void assign_n(int, int, const Functor & fnc) noexcept
  {
    fnc(0);
  }

  template<class It>
  void assign(It first, It last) noexcept
  {
    assign_n(1, 1, make_assign_range(first, last, alloc));
  }
};

int main()
{
  {
    Buffer buffer;
    buffer.assign(buffer.begin(), buffer.end());
  }
  return Iterator::destroyed > 0 ? 0 : 1;
}
