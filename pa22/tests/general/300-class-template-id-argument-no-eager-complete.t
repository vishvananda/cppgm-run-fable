template<class T>
struct next_type
{
  typedef typename T::next type;
};

template<class T>
struct base
{
  typedef int type;
};

template<class T>
struct use : base<next_type<T> >
{
};

struct no_next
{
};

typedef use<no_next>::type result_type;

int main()
{
  result_type value = 0;
  return value;
}
