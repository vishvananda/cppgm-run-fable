// VALIDATION: compile-pass

struct item
{
  int value;
};

struct buffer
{
  item data[4];

  operator item*()
  {
    return data;
  }

  operator const item*() const
  {
    return data;
  }
};

int read(buffer & b, int i)
{
  return b[i].value;
}

int read_const(const buffer & b, int i)
{
  return b[i].value;
}

int write(buffer & b)
{
  b[2].value = 11;
  return b[2].value;
}

int main()
{
  buffer b;
  b.data[1].value = 7;
  return read(b, 1) + write(b) + read_const(b, 2) == 29 ? 0 : 1;
}
