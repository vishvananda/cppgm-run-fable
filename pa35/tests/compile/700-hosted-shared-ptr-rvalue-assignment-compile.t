#include <memory>
#include <type_traits>
static_assert(std::is_same<std::shared_ptr<int>::element_type, int>::value, "shared_ptr element_type");
struct Owner {
  struct Blacklist {};
  typedef std::shared_ptr<const Blacklist> BlacklistPtr;
  void add(BlacklistPtr & shared_blacklist) { shared_blacklist = std::make_shared<Blacklist>(); }
};
bool shared_ptr_assignment_anchor()
{
  Owner owner;
  Owner::BlacklistPtr ptr;
  owner.add(ptr);
  return static_cast<bool>(ptr);
}
static_assert(sizeof(&shared_ptr_assignment_anchor) > 0, "shared_ptr assignment body anchor");
