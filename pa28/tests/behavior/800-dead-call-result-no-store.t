function @one() -> i64 {
  block ^entry:
    return i64 7
}

function @id(%a : i64) -> i64 {
  block ^entry:
    return i64 %a
}

function @main() -> i64 [role=entry] {
  block ^entry:
    %x = call i64 @one()
    %dead = call i64 @id(99)
    %bad = cmp ne i64 %x, 7
    return i64 %bad
}
