#include <string>
#include <vector>
#include <type_traits>
static_assert(std::is_same<std::vector<int>::value_type, int>::value, "<vector> value_type");
namespace parser_trace {
namespace {
struct Event { const char * category; std::string location; std::string message; };
}
std::vector<Event> events_;
void note() { std::vector<Event>::const_iterator last = events_.begin(); (void)last; }
}
static_assert(sizeof(&parser_trace::note) > 0, "const_iterator body anchor");
