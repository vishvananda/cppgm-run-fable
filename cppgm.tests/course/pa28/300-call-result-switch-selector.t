function @pick() -> i32 {
  block ^entry:
    return i32 -7
}

function @spoil() -> i32 {
  block ^entry:
    return i32 9
}

function @classify() -> i32 {
  block ^entry:
    %t0 = call i32 @pick()
    %t1 = call i32 @spoil()
    switch %t0, ^dflt, -7:^hit, 9:^wrong

  block ^hit:
    return i32 0

  block ^wrong:
    return i32 2

  block ^dflt:
    return i32 1
}

function @main() -> i32 [role=entry, binding=strong] {
  block ^entry:
    %r = call i32 @classify()
    return i32 %r
}
