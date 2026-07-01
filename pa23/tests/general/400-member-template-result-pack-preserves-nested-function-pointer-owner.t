struct error_code {
};

template<class F, class A, class P>
struct bind_like {
};

struct slot {
  template<class Handler, class... Args>
  Handler & emplace(Args &&...) {
    static Handler h;
    return h;
  }
};

template<class Handler, class Timer, class... Signatures>
struct op {
  struct proxy {
    proxy() {
    }

    explicit proxy(op *) {
    }
  };

  template<class Initiation, class... Args>
  void start(Initiation &, Args &&...) {
    slot s;
    proxy * p = &s.template emplace<proxy>(this);
    (void)p;
  }
};

struct timer {
};

struct init {
};

void cancel(int *, error_code const &);

int main()
{
  op<bind_like<void (*)(int *, error_code const &), int * &, int>,
     timer,
     void(error_code)> operation;
  init starter;
  operation.start(starter);
  return 0;
}
