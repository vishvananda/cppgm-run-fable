declare function @g__tls_wrapper() -> ptr [binding=strong, object=_ZTW1g, tls_for=@g]

global @g : i64 [storage=thread_local, binding=strong, object=_Z1g] = 0

function @f(%a : i64, %b : i64, %c : i64, %d : i64, %e : i64, %x : i64) -> i64 [binding=strong, object=_Z1fllllll] {
  block ^entry:
    %t1 = binary add i64 %a, 1
    %t2 = binary add i64 %b, 2
    %t3 = binary add i64 %c, 3
    %t4 = binary add i64 %d, 4
    %t5 = binary add i64 %e, 5
    %z = binary add i64 %x, 1
    store i64 %z, @g
    %s1 = binary add i64 %t1, %t2
    %s2 = binary add i64 %s1, %t3
    %s3 = binary add i64 %s2, %t4
    %s4 = binary add i64 %s3, %t5
    %s5 = binary add i64 %s4, %z
    return i64 %s5
}
function @main() -> i32 [role=entry, binding=strong, keep_alias=yes] {
  block ^entry:
    %r = call i64 @f(1, 2, 3, 4, 5, 6)
    %t = convert trunc i32 i64 %r
    return i32 %t
}
