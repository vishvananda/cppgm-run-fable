struct Payload {
  Payload();
  Payload(Payload &&);
  Payload & operator=(Payload &&);
  ~Payload();
};

struct Tok {
  int type;
  Payload data;
};

struct Cursor {
  Tok get();
};

struct Macroizer {
  bool active();
  Tok get();
};

extern int g_runtime_branch;
bool complete(Cursor & cursor);
bool process(int type, const Payload & data, bool allow_macro_start);

Tok preproc_get(Cursor & cursor, Macroizer & macroizer)
{
  if(complete(cursor)) {
    return Tok{5};
  }
  Tok token;
  bool output = false;
  do {
    if(g_runtime_branch == 1) {
      token = cursor.get();
      output = process(token.type, token.data, false);
    } else if(macroizer.active()) {
      token = macroizer.get();
      if(token.type == 5) {
        continue;
      }
      output = process(token.type, token.data, false);
    } else {
      token = cursor.get();
      output = process(token.type, token.data, true);
    }
  } while(output == false);
  if(token.type == 1) {
    token.type = 0;
  }
  return token;
}
