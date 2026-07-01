// VALIDATION: compile-pass
// Boost.Algorithm find_backward reduction: a member function template from a
// class-template base must merge the owner-prefixed declaration specialization
// with the materialized empty-pack definition specialization.

struct Node {};

template<class T>
struct Base {
  typedef Node * node_pointer;

  template<class... Args>
  node_pointer create(Node *, Node *, Args&&...) {
    return 0;
  }
};

template<class T>
struct List : private Base<T> {
  typedef Base<T> base;
  typedef typename base::node_pointer node_pointer;

  void resize();
};

template<class T>
void List<T>::resize() {
  node_pointer p = this->create(nullptr, nullptr);
  (void)p;
}

int main() {
  List<int> l;
  l.resize();
  return 0;
}
