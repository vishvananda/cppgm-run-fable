// VALIDATION: compile-pass
// decltype(T()) in a default template argument should SFINAE away
// class types that cannot be default-constructed.

struct no_default_ctor
{
  no_default_ctor(int);
};

struct default_ctor
{
  default_ctor();
};

typedef char yes_type;
struct no_type { char bytes[2]; };

template<class T, class = decltype(T())>
yes_type default_construct_probe(int);

template<class>
no_type default_construct_probe(...);

static_assert(sizeof(default_construct_probe<default_ctor>(0)) == sizeof(yes_type),
              "default-constructible class should use primary probe");
static_assert(sizeof(default_construct_probe<no_default_ctor>(0)) == sizeof(no_type),
              "non-default-constructible class should use fallback probe");
static_assert(sizeof(default_construct_probe<void>(0)) == sizeof(yes_type),
              "void() is a valid unevaluated expression");

int main()
{
  return 0;
}
