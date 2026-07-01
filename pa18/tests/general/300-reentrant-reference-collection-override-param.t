// VALIDATION: compile-pass

struct pa18_reentrant_override_stream
{
};

struct pa18_reentrant_override_entry
{
};

struct pa18_reentrant_override_base
{
  enum entry_kind
  {
    entry_info
  };

  virtual ~pa18_reentrant_override_base()
  {
  }

  virtual void start(pa18_reentrant_override_stream&,
                     pa18_reentrant_override_entry const&,
                     entry_kind) = 0;
};

template<class T>
struct pa18_reentrant_override_holder
{
  typename T::tag* p;
};

struct pa18_reentrant_override_derived : pa18_reentrant_override_base
{
  typedef int tag;

  void start(pa18_reentrant_override_stream&,
             pa18_reentrant_override_entry const&,
             entry_kind) override;

  pa18_reentrant_override_holder<pa18_reentrant_override_derived> member;
};

void pa18_reentrant_override_derived::start(pa18_reentrant_override_stream&,
                                            pa18_reentrant_override_entry const&,
                                            entry_kind)
{
}

int main()
{
  pa18_reentrant_override_derived d;
  (void)d;
  return 0;
}
