#include <rtems.h>

#include <stdio.h>
#include <stdlib.h>

#include "rtfs_config.h"


#ifdef ENABLE_UNIT_TEST
#include "rtfs_test.h"
#endif


rtems_task Init(rtems_task_argument ignored)
{
#ifdef ENABLE_UNIT_TEST
    runAllTests();
#endif


    rtems_shutdown_executive(0);
}
