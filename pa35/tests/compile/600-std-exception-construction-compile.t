#include <exception>

static_assert(__is_constructible(std::exception), "std::exception default construction");
static_assert(__is_constructible(std::exception, const std::exception&), "std::exception copy construction");
