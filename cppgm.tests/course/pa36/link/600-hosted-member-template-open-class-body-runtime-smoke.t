# hosted member-template deferred-body smoke: a member-template
# specialization demanded while a class-template specialization
# instantiates inside a still-open class (the Box<Node> member field
# completes Box<Node> before Node's own definition closes) must bind
# its body after the unit's forward pass, when Node's implicit copy
# assignment exists - not at the out-of-class definition capture
# during the specialization replay.
