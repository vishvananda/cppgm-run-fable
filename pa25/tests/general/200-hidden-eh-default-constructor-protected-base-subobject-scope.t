class noncopyable {
protected:
  noncopyable() noexcept {}
  ~noncopyable() noexcept {}
};

class service_maker : private noncopyable {
public:
  service_maker() noexcept {}
  ~service_maker() noexcept {}
  void make() const noexcept {}
};

struct maker : service_maker {
  maker() noexcept {}
  ~maker() noexcept {}
};

struct context {
  explicit context(const service_maker& initial) noexcept {
    initial.make();
  }
};

struct io_context : context {
  io_context(const service_maker& initial) noexcept : context(initial) {}
};

int main() {
  io_context ioc{maker{}};
  return 0;
}
