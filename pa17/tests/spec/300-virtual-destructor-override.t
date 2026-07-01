// VALIDATION: compile-pass
// N3485 focus: 10.3 [class.virtual]

class YB { public: virtual int f() noexcept { return 1; } virtual ~YB() noexcept {} };
class YD : public YB { public: int f() noexcept override { return 5; } ~YD() noexcept override {} };
int main() { YD d; return d.f(); }
