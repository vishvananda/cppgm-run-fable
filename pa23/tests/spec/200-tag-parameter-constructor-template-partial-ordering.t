// VALIDATION: compile-pass
// N3485 focus: 14.8.2.4 [temp.deduct.partial]

template<class D>
struct tag
{
};

struct Count
{
  int * pi_;

#line 1 "shared_count.hpp"
  template<class P, class D>
  Count(P p, D)
    : pi_(0)
  {
    pi_ = p;
  }

#line 1 "shared_count.hpp"
  template<class P, class D>
  Count(P, tag<D>)
    : pi_(0)
  {
  }
};
#line 35 "pa23/tests/spec/200-tag-parameter-constructor-template-partial-ordering.t"

struct Deleter
{
  void operator()(int *)
  {
  }

  static void operator_fn(int *)
  {
  }
};

struct Holder
{
  Count count;

  template<class P, class D>
  Holder(P p, D d)
    : count(p, static_cast<D&&>(d))
  {
  }
};

template<class P, class D>
void make_holder(P p, D d)
{
  Holder h(p, static_cast<D&&>(d));
}

int main()
{
  Count first((int *)0, Deleter());
  make_holder((int *)0, tag<Deleter>());
  return 0;
}
