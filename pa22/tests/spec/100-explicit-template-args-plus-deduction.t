// VALIDATION: compile-pass
// N3485 focus: 14.8.1 [temp.arg.explicit], 14.8.2 [temp.deduct]

template<typename T, typename U>
int pair_sum(T left, U right)
{
  return static_cast<int>(left + right);
}

int main()
{
  return pair_sum<int>(2, 3.0) == 5 ? 0 : 1;
}
