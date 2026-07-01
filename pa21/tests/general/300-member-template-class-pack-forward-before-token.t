// VALIDATION: compile-pass
// A class-template parameter pack must bind to the matching call arguments
// before the trailing member-template token parameter.

struct error_code
{
  int value;
  error_code(int v = 0) : value(v) {}
};

struct string
{
  int value;
  string(int v = 0) : value(v) {}
};

int combine(error_code e, string s)
{
  return e.value + s.value;
}

template<class... Args>
struct sender
{
  template<class Token>
  int send(Args... args, Token token)
  {
    return combine(static_cast<Args&&>(args)...) + token;
  }
};

int main()
{
  sender<error_code, string> s;
  int token = 1;
  return s.send(error_code(2), string(3), token) != 6;
}
