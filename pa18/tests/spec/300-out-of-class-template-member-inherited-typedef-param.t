// VALIDATION: compile-pass
// N3485 focus: 14.5.1 [temp.class], 10 [class.derived], 3.4.1 [basic.lookup.unqual]

template<class T>
struct holder {
};

struct base {
  typedef int catalog;
};

template<class T>
struct messages : base {
  typedef holder<T> string_type;

  string_type get(catalog, const string_type&) const;
};

template<class T>
typename messages<T>::string_type
messages<T>::get(catalog, const string_type& value) const
{
  return value;
}

int main()
{
  return 0;
}
