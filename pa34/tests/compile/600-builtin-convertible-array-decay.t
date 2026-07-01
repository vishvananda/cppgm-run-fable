struct text {
  text(char const *);
};

typedef char literal_type[6];

static_assert(__is_convertible(literal_type, char *), "");
static_assert(__is_convertible(literal_type, char const *), "");
static_assert(__is_convertible(literal_type, text), "");
static_assert(__is_constructible(text, literal_type), "");
static_assert(!__is_convertible(literal_type, char (&)[6]), "");

int main() {
  return 0;
}
