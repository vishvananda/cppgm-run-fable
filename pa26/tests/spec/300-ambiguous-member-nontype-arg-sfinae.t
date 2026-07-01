// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], 14.8.2.5 [temp.deduct.type]
// Substituting an explicit function-template argument can make a non-type
// template argument's member lookup ambiguous. That invalid immediate context
// drops the candidate instead of making the call ill-formed.

template <class T, T>
struct check
{};

struct yes
{
  char c;
};

struct no
{
  char c[2];
};

struct fallback
{
  void f();
};

template <class T>
struct derived : T, fallback
{};

struct high
{};

struct low
{
  low(high);
};

template <class>
no helper(low);

template <class T>
yes helper(high, check<void (fallback::*)(), &derived<T>::f> * = 0);

struct has_f
{
  void f();
};

struct no_f
{};

int main()
{
  if(sizeof(helper<has_f>(high())) != sizeof(no)) {
    return 1;
  }
  return sizeof(helper<no_f>(high())) == sizeof(yes) ? 0 : 2;
}
