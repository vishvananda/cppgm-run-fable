class archive_access;

class archive_base {
protected:
  void end_preamble() {}
};

class archive_impl : public archive_base {
  friend class archive_access;
};

class archive : public archive_impl {};

class archive_access {
public:
  static void save(archive & ar)
  {
    ar.end_preamble();
  }
};

int main()
{
  archive ar;
  archive_access::save(ar);
  return 0;
}
