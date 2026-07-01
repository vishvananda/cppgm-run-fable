// VALIDATION: compile-pass
// N3485 focus: 14.6.2.1 [temp.dep.type], 14.5.5 [temp.class.spec]
// When a member function template of a class-template specialization names a
// nested class template without qualification, lookup must check the current
// specialization's member scope before inherited base scopes.

template <typename Token>
struct async_result;

template <typename T>
struct binder
{
};

template <typename T>
struct appended
{
};

struct initiation
{
  void operator()(int)
  {
  }
};

template <typename T>
struct async_result<binder<T> >
{
  template <typename Initiation>
  struct init_wrapper : Initiation
  {
    explicit init_wrapper(Initiation init)
      : Initiation(init)
    {
    }

    void operator()(const binder<T>&)
    {
    }
  };
};

template <typename T>
struct async_result<appended<T> > : async_result<T>
{
  template <typename Initiation>
  struct init_wrapper : Initiation
  {
    explicit init_wrapper(Initiation init)
      : Initiation(init)
    {
    }

    void operator()(int value)
    {
      static_cast<Initiation&&>(*this)(value);
    }
  };

  template <typename Initiation>
  static void initiate(Initiation init)
  {
    init_wrapper<Initiation> wrapper(init);
    wrapper(1);
  }
};

int main()
{
  initiation init;
  async_result<appended<binder<int> > >::initiate<initiation>(init);
  return 0;
}
