set -eu
nm "__OBJ1__" | c++filt | grep -Eq '^[0-9A-Fa-f]+ [TtWw] \(anonymous namespace\)::Box::helper\(int\)$'
if nm "__OBJ1__" | c++filt | grep -Eq '^ +U \(anonymous namespace\)::Box::helper\(int\)$'; then
  echo unexpected_undefined_helper
  exit 1
fi
echo anon_static_helper_defined 1
