// VALIDATION: compile-pass

struct bits
{
  unsigned u;

  auto remove(unsigned exponent_bits) const noexcept -> unsigned
  {
    return u + exponent_bits;
  }
};

int main()
{
  bits b;
  b.u = 2;
  return b.remove(1) == 3 ? 0 : 1;
}
