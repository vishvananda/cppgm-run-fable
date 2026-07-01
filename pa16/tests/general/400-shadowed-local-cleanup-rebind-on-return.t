struct S {
  int *p;

  S() noexcept : p(0) {}
  S(const S &o) noexcept : p(o.p) {}

  S &operator=(const S &o) noexcept {
    p = o.p;
    return *this;
  }

  ~S() noexcept {}

  operator bool() const noexcept {
    return p != 0;
  }
};

bool f(S pattern, S actual) noexcept {
  S pattern_cv_inner;
  S actual_cv_inner;
  S pattern_base = pattern;
  S actual_base = actual;
  if (pattern_base) {
    S deduced_type = actual;
    S actual_cv_inner;
    S pattern_cv_inner;
    if (actual_base) {
      deduced_type = actual_cv_inner;
    }
    return true;
  }
  return false;
}
