extern int a[];
int a[10];
extern int a[];
namespace Q { extern int arr[]; extern volatile int arr2[]; }
int Q::arr[12];
volatile int Q::arr2[7];
