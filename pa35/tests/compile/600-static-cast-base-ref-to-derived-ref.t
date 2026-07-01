#include <string>

struct Derived;

struct Base {
  std::size_t hash() const;
};

struct Derived : Base {
  std::size_t value;
};

std::size_t Base::hash() const
{
  return static_cast<Derived const&>(*this).value;
}

std::size_t static_cast_base_anchor(Derived & d)
{
  return d.hash();
}
static_assert(sizeof(&static_cast_base_anchor) > 0, "base-to-derived static_cast body anchor");
