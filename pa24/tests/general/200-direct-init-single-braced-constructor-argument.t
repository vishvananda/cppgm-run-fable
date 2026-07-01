// VALIDATION: compile-pass
// N3485 focus: 5.2.3 explicit type conversion and 8.5.4 list-initialization.
// In T({a, b}), the braced-init-list is a single direct-initialization argument.

struct error_code
{
  int value;
};

template<class Signature>
struct completion_message;

template<class R, class Arg>
struct completion_message<R(Arg)>
{
  template<class T>
  completion_message(int, T&& arg) : value(static_cast<T&&>(arg)) {}

  Arg value;
};

template<class Signature>
struct completion_payload
{
  completion_payload(completion_message<Signature>&& msg)
      : value(static_cast<completion_message<Signature>&&>(msg).value) {}

  error_code value;
};

completion_payload<void(error_code)> make(error_code ec)
{
  return completion_payload<void(error_code)>({0, ec});
}

int main()
{
  error_code ec = {7};
  return make(ec).value.value == 7 ? 0 : 1;
}
