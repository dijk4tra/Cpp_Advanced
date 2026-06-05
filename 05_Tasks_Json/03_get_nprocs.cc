#include "common.h"
#include <sys/sysinfo.h>

int main()
{
    printf("processors: %d\n", get_nprocs());
}
