struct nat {
  nat() = delete;
  nat(const nat &) = delete;
  nat & operator=(const nat &) = delete;
  ~nat() = delete;
};

template<class T, class U, class = void>
struct core_convertible {
  static const bool value = false;
};

template<class T, class U>
struct core_convertible<T, U,
    decltype(static_cast<void (*)(U)>(0)(static_cast<T (*)()>(0)()))> {
  static const bool value = true;
};

static_assert(!core_convertible<nat, bool>::value,
              "invalid conversion should be dropped by substitution failure");
static_assert(core_convertible<int, bool>::value,
              "valid conversion should select the partial specialization");

int main()
{
  return core_convertible<int, bool>::value ? 0 : 1;
}
