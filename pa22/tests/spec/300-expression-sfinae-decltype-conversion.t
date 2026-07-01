// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], expression SFINAE in a dependent decltype.

template<typename T>
T && declval();

struct source
{
};

struct target
{
};

template<typename From, typename To>
class is_convertible
{
  typedef char one;
  typedef int two;

  template<typename To1>
  static void test_aux(To1);

  template<typename From1, typename To1>
  static decltype(test_aux<To1>(declval<From1>()), one()) test(int);

  template<typename, typename>
  static two test(...);

public:
  static const bool value = sizeof(test<From, To>(0)) == sizeof(one);
};

static_assert(is_convertible<source, source>::value,
              "same class is convertible");
static_assert(!is_convertible<source, target>::value,
              "unrelated classes are not convertible");

int main()
{
  return is_convertible<source, target>::value ? 1 : 0;
}
