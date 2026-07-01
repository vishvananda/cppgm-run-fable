// VALIDATION: compile-pass
// N3485 focus: 14.8.2.2 [temp.deduct.funcaddr]

struct context {};

struct service {};

service global_service;

struct service_registry
{
  typedef service *(*factory_type)(context &, void *);

  template<typename Service>
  factory_type get()
  {
    factory_type factory = &service_registry::create<Service, context>;
    return factory;
  }

  template<typename Service, typename Owner, typename... Args>
  static service * create(context &, void *, Args&&...);
};

template<typename Service, typename Owner, typename... Args>
service * service_registry::create(context &, void *, Args&&...)
{
  return &global_service;
}

int main()
{
  context ctx;
  service_registry registry;
  service_registry::factory_type factory = registry.get<service>();
  return factory(ctx, &ctx) == &global_service ? 0 : 1;
}
