# hosted conditional-noexcept member terminate smoke: an in-class
# member body of a class-template specialization binds inside the
# class's own replay window; its deferred noexcept(constant) fact
# must still reach the definition node, or the body compiles without
# the 15.4p9 terminate barrier while call sites trust the
# specification - the throw below then unwinds into main's catch
# instead of calling std::terminate. Reduced from the PA39 CWG 1330
# deferral audit.
