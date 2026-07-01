struct Item {
  int value;
};

int run(Item * items, int count)
{
  const Item * parameter = items;
  auto outer = [items, count]() -> int
  {
    auto match = [&](const Item * source) -> int
    {
      const Item & parameter = source[count - 1];
      return parameter.value;
    };
    return match(items);
  };
  return outer() + parameter[0].value;
}

int main()
{
  Item items[1] = {{7}};
  return run(items, 1);
}
