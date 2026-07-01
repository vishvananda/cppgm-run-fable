// VALIDATION: compile-pass

enum class tag_t {
  to_nearest
};

struct policy {
  static constexpr auto tag = tag_t::to_nearest;
};

int main() {
  return policy::tag == tag_t::to_nearest ? 0 : 1;
}
