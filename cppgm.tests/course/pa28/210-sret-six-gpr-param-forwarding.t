global @A = {
  i64 0
}
global @B = {
  i64 0
}
global @C = {
  i64 0
}
global @D = {
  i64 0
}

function @target(%ret : ptr [pass=indirect_result], %this : ptr, %cls : ptr, %idx : i32, %base : u8, %addr : ptr, %vec : ptr) -> void {
  block ^entry:
    %a = addr @A
    %b = addr @B
    %c = addr @C
    %d = addr @D
    %w1 = cmp ne ptr %this, %a
    %w2 = cmp ne ptr %cls, %b
    %w3 = cmp ne i32 %idx, 7
    %w4 = cmp ne u8 %base, 1
    %w5 = cmp ne ptr %addr, %c
    %w6 = cmp ne ptr %vec, %d
    %s1 = binary add i64 %w1, %w2
    %s2 = binary add i64 %s1, %w3
    %s3 = binary add i64 %s2, %w4
    %s4 = binary add i64 %s3, %w5
    %s5 = binary add i64 %s4, %w6
    store i64 %s5, %ret
    return void
}

function @forward(%ret : ptr [pass=indirect_result], %arg1 : ptr, %arg2 : ptr, %arg3 : i32, %arg4 : u8, %arg5 : ptr, %arg6 : ptr) -> void {
  block ^entry:
    %t1 = index i8 %arg1, -136
    call void @target(%ret, %t1, %arg2, %arg3, %arg4, %arg5, %arg6)
    return void
}

function @main() -> i64 [role=entry] {
  slot $r : i64

  block ^entry:
    %r = addr $r
    %a = addr @A
    %athis = index i8 %a, 136
    %b = addr @B
    %c = addr @C
    %d = addr @D
    call void @forward(%r, %athis, %b, 7, 1, %c, %d)
    %v = load i64 $r
    return i64 %v
}
