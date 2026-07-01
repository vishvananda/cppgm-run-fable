// VALIDATION: compile-pass

struct array_reference_cv_stream
{
};

namespace array_reference_cv_detail
{
  template<typename T>
  struct print_helper_t
  {
    explicit print_helper_t(T const& t)
        :
        ref(t)
    {
    }

    T const& ref;
  };

  template<typename T>
  print_helper_t<T> print_helper(T const& t)
  {
    return print_helper_t<T>(t);
  }

  template<typename T>
  array_reference_cv_stream& operator<<(array_reference_cv_stream& out,
                                        print_helper_t<T> const&)
  {
    return out;
  }
}

class array_reference_cv_lazy
{
public:
  virtual ~array_reference_cv_lazy()
  {
  }

  virtual array_reference_cv_stream& operator()(array_reference_cv_stream& out) const
  {
    return out;
  }

protected:
  explicit array_reference_cv_lazy(bool = true)
  {
  }
};

template<typename PrevType, typename T, typename StorageT = T const&>
class array_reference_cv_lazy_impl : public array_reference_cv_lazy
{
public:
  array_reference_cv_lazy_impl(PrevType const& prev, T const& value)
      :
      array_reference_cv_lazy(false),
      prev_(prev),
      value_(value)
  {
  }

  array_reference_cv_stream& operator()(array_reference_cv_stream& out) const override
  {
    return prev_(out) << array_reference_cv_detail::print_helper(value_);
  }

private:
  PrevType const& prev_;
  StorageT value_;
};

template<typename T>
array_reference_cv_lazy_impl<array_reference_cv_lazy, T>
operator<<(array_reference_cv_lazy const& prev, T const& value)
{
  return array_reference_cv_lazy_impl<array_reference_cv_lazy, T>(prev, value);
}

void force(array_reference_cv_lazy const& lazy, array_reference_cv_stream& out)
{
  (lazy << "abc")(out);
}

int main()
{
  return 0;
}
