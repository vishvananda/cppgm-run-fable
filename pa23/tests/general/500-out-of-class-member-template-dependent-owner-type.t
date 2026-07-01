// VALIDATION: compile-pass
// N3485 focus: 14.5.2 member templates, 14.6.2.1 dependent types.
// An out-of-class member-template definition can spell a nested dependent type
// through the current class template-id even when the declaration used the
// member name directly.

struct mutex_type {};

template<class Mutex>
struct service
{
  template<class Traits, class... Signatures>
  struct implementation_type;

  template<class Traits, class... Signatures, class Handler>
  int receive(implementation_type<Traits, Signatures...>& impl, Handler&& handler);
};

template<class Mutex>
template<class Traits, class Signature>
struct service<Mutex>::implementation_type<Traits, Signature>
{
  int value;
};

template<class Mutex>
template<class Traits, class... Signatures, class Handler>
int service<Mutex>::receive(
    service<Mutex>::implementation_type<Traits, Signatures...>& impl,
    Handler&& handler)
{
  return impl.value + handler();
}

struct traits {};

struct handler
{
  int operator()() { return 5; }
};

int main()
{
  service<mutex_type> svc;
  service<mutex_type>::implementation_type<traits, void(int)> impl = {7};
  handler h;
  return svc.receive(impl, h) == 12 ? 0 : 1;
}
