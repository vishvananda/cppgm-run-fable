struct Entry
{
    int id;
    const char* name;
    constexpr Entry(int i, const char* n) : id(i), name(n) {}
};

template<class T>
struct Tables
{
    static constexpr Entry rows[2] = { Entry(1, "one"), Entry(2, "two") };
};

template<class T>
constexpr Entry Tables<T>::rows[2];

static_assert(Tables<int>::rows[1].id == 2, "rows");

const Entry* rows() { return Tables<int>::rows; }

int main()
{
    return (rows()[1].id + rows()[0].name[0]) == (2 + 'o') ? 0 : 1;
}
