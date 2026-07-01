struct sample
{
  int n;
};

template<class Reference, class Pointer>
struct operator_arrow_dispatch
{
  struct proxy
  {
    explicit proxy(Reference const& x) : m_ref(x) {}

    int value() const
    {
      return m_ref.n;
    }

    Reference m_ref;
  };

  typedef proxy result_type;

  static result_type apply(Reference const& x)
  {
    return result_type(x);
  }
};

inline int call_operator_prefix_owner(sample const& x)
{
  return operator_arrow_dispatch<sample, sample*>::apply(x).value();
}
