// VALIDATION: compile-pass
// N3485 focus: 10.3 [class.virtual]

class YB { public: virtual int g() noexcept { return 1; } virtual ~YB() noexcept {} };
class YD : public YB { public: virtual int g() noexcept final { return 4; } };
int f(YB &b) noexcept { return b.g(); }
int main() { YD d; return f(d); }
