// VALIDATION: compile-pass
// Out-of-class member class-template partials keep the dependent owner.

template<typename Mutex>
struct channel_service
{
  struct base_implementation_type
  {
  };

  template<typename Traits, typename... Signatures>
  struct implementation_type;
};

template<typename Mutex>
template<typename Traits, typename R>
struct channel_service<Mutex>::implementation_type<Traits, R()>
  : channel_service<Mutex>::base_implementation_type
{
  typedef R result_type;

  int value() const
  {
    return 7;
  }
};

int main()
{
  channel_service<int>::implementation_type<int, void()> impl;
  return impl.value() == 7 ? 0 : 1;
}
