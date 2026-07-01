set -eu
nm "__OBJ1__" | c++filt | grep -Eq 'Box<int>::probe<false>\(\) const'
nm "__OBJ1__" | c++filt | grep -Eq 'Box<int>::probe<true>\(\) const'
echo bool_member_template_specializations 2
