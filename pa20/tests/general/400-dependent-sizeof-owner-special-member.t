// VALIDATION: compile-pass
// N3485 focus: 5.19 [expr.const], 14.3.2 [temp.arg.nontype]
// A dependent non-type template argument may reuse a sizeof expression in the
// current template owner, including when the same dependent base type is named
// in an out-of-class special member definition.

typedef unsigned long size_t;

template<size_t Words, size_t Size>
struct storage
{
  storage();
  storage &operator&=(const storage &rhs);
};

template<size_t Words, size_t Size>
storage<Words, Size>::storage()
{
}

template<size_t Words, size_t Size>
storage<Words, Size> &storage<Words, Size>::operator&=(const storage &rhs)
{
  (void)rhs;
  return *this;
}

template<size_t Size>
struct bits
    : storage<Size == 0 ? 0 : (Size - 1) / (sizeof(size_t) * 8) + 1,
              Size>
{
  typedef storage<Size == 0 ? 0 : (Size - 1) / (sizeof(size_t) * 8) + 1,
                  Size> base;
  bits &operator&=(const bits &rhs);
};

template<size_t Size>
bits<Size> &bits<Size>::operator&=(const bits &rhs)
{
  base::operator&=(rhs);
  return *this;
}

bits<1> *get_bits();

int main()
{
  bits<1> *value = get_bits();
  return &(value->operator&=(*value)) == value ? 0 : 1;
}
