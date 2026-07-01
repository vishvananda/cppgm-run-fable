#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

enum Access
{
  PUBLIC,
  PROTECTED,
  PRIVATE
};

struct Node;

struct Subobject
{
  const Node * type = nullptr;
  size_t offset = 0;
  Access access = PUBLIC;
};

struct Base
{
  Node * type = nullptr;
  size_t offset = 0;
  bool is_virtual = false;
  Access access = PUBLIC;
};

struct Node
{
  string qualified_name;
  vector<Subobject> complete_subobjects;
  vector<Base> bases;
};

Access combine_access(Access inherited, Access edge)
{
  if(inherited == PRIVATE || edge == PRIVATE) {
    return PRIVATE;
  }
  if(inherited == PROTECTED || edge == PROTECTED) {
    return PROTECTED;
  }
  return PUBLIC;
}

template<typename Callback>
size_t collect_base_paths_impl(const Node & current,
                               const Node * target,
                               size_t offset,
                               Access access,
                               set<const Node *> & visited_virtual,
                               const Callback & callback)
{
  size_t matches = 0;
  if(!target) {
    return matches;
  }
  if(!current.complete_subobjects.empty()) {
    for(size_t i = 0; i < current.complete_subobjects.size(); ++i) {
      const Subobject & subobject = current.complete_subobjects[i];
      if(subobject.type != target) {
        continue;
      }
      callback(offset + subobject.offset,
               combine_access(access, subobject.access));
      ++matches;
    }
    return matches;
  }
  if(&current == target) {
    callback(offset, access);
    ++matches;
  }

  for(size_t i = 0; i < current.bases.size(); ++i) {
    const Base & base = current.bases[i];
    if(base.is_virtual && !visited_virtual.insert(base.type).second) {
      continue;
    }
    matches += collect_base_paths_impl(*base.type,
                                       target,
                                       offset + base.offset,
                                       combine_access(access, base.access),
                                       visited_virtual,
                                       callback);
  }
  return matches;
}

template<typename Callback>
size_t collect_base_paths(const Node & current,
                          const Node * target,
                          size_t offset,
                          Access access,
                          const Callback & callback)
{
  set<const Node *> visited_virtual;
  return collect_base_paths_impl(current, target, offset, access, visited_virtual, callback);
}

bool find_unique_base_path(const Node & current,
                           const Node * target,
                           size_t & out_offset,
                           Access & out_access)
{
  bool found = false;
  size_t matches =
      collect_base_paths(current, target, 0, PUBLIC,
                         [&found, &out_offset, &out_access](size_t offset, Access access)
                         {
                           if(!found) {
                             found = true;
                             out_offset = offset;
                             out_access = access;
                           }
                         });
  if(matches == 1 && found) {
    return true;
  }
  if(matches > 1) {
    ostringstream out;
    out << "ambiguous base class path";
    out << " [current " << current.qualified_name << "]";
    out << " [target " << (target ? target->qualified_name : string("<null>")) << "]";
    throw logic_error(out.str());
  }
  return false;
}

bool unique_base_path_anchor()
{
  Node root;
  size_t offset = 0;
  Access access = PUBLIC;
  return find_unique_base_path(root, &root, offset, access);
}
static_assert(sizeof(&unique_base_path_anchor) > 0, "base path body anchor");
