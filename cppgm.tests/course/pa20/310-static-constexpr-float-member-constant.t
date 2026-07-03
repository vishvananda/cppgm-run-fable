struct Consts
{
    static constexpr double scale = 2.5;
    static constexpr const char* tag = "hi";
};

static_assert(Consts::scale > 2.0, "scale");

double scale() { return Consts::scale; }

int main()
{
    return scale() == 2.5 ? 0 : 1;
}
