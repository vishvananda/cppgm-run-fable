#include <vector>
#include <type_traits>
static_assert(std::is_same<std::vector<int>::value_type, int>::value, "<vector> value_type");
struct Incomplete;
struct Context {
  virtual void f(const std::vector<Incomplete>& args) = 0;
  virtual const std::vector<Incomplete>& g() const = 0;
};
