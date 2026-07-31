# hosted class-throw payload-window region smoke: a class-type throw
# splits its dispatch regions around __cxa_allocate_exception, and the
# eh_end closing the allocation's region must also retire the
# synthetic throw-payload window the region analysis pushed at the
# allocation call. Leaving the window armed shifts the marker LIFO so
# the allocation region leaks onto the stack, and the flat cleanup
# pad's resume gets covered by it - the unwinder re-enters the leaked
# region's pad and destroys the enclosing locals a second time.
# Reduced from SemExprAnalyzer::AnalyzeStaticMethodCall, where the
# self-built compiler crashed releasing a vector<ConversionSource>
# element's shared_ptr control block twice.
