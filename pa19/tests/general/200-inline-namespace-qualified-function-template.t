// VALIDATION: compile-pass

namespace boost
{
namespace asio
{
inline namespace v1
{
namespace execution
{
template<class T>
struct box
{
};

template<class T>
int pick(box<T>&)
{
  return 0;
}
}
}
}
}

using namespace boost::asio;

int main()
{
  execution::box<int> b;
  return execution::pick(b);
}
