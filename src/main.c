#include <rtems.h>
#include <rtems/test-info.h>

#include <stdio.h>
#include <stdlib.h>

#include "rtfs_config.h"


#ifdef ENABLE_UNIT_TEST
#include "rtfs_test.h"
#endif


rtems_task Init(rtems_task_argument ignored)
{
#ifdef ENABLE_UNIT_TEST
    rtfsRunAllTests();
#endif

#ifdef ENABLE_COVERAGE
    rtems_test_gcov_dump_info();
#endif


    rtems_shutdown_executive(0);
}
