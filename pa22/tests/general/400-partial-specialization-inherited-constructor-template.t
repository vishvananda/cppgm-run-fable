// VALIDATION: compile-pass
// Constructor-template participation must survive when inherited through a
// class-template partial specialization.

struct impl {
  template <class Self>
  void operator()(Self&) {
  }
};

struct work {
};

struct handler {
};

template <class Impl, class Work, class Handler, class... Signatures>
struct op;

template <class Impl, class Work, class Handler>
struct op<Impl, Work, Handler> {
  template <class I, class W, class H>
  op(I&& impl, W&& work, H&& handler)
      : impl_(static_cast<I&&>(impl)),
        work_(static_cast<W&&>(work)),
        handler_(static_cast<H&&>(handler)) {
  }

  op(op&& other)
      : impl_(static_cast<Impl&&>(other.impl_)),
        work_(static_cast<Work&&>(other.work_)),
        handler_(static_cast<Handler&&>(other.handler_)) {
  }

  Impl impl_;
  Work work_;
  Handler handler_;
};

template <class Impl, class Work, class Handler, class R, class... Args>
struct op<Impl, Work, Handler, R(Args...)> : op<Impl, Work, Handler> {
  using op<Impl, Work, Handler>::op;

  void run() {
    this->impl_(*this);
  }
};

int main() {
  impl i;
  work w;
  handler h;
  op<impl, work, handler, void(int)> x(
      static_cast<impl&&>(i),
      static_cast<work&&>(w),
      static_cast<handler&&>(h));
  x.run();
  return 0;
}
