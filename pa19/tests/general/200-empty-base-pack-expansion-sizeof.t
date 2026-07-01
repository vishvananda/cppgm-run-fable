// VALIDATION: compile-pass

template<class T>
struct store
{
  T value;
};

template<class... T>
struct data : store<T>...
{
};

template<class... T>
struct tuple : data<T...>
{
};

int main()
{
  typedef tuple<> empty;
  typedef tuple<empty> nested;
  return sizeof(nested) > 0 ? 0 : 1;
}
