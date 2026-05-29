#
# Hello world Waf script
#

from __future__ import print_function

rtems_version = "6"

try:
    import rtems_waf.rtems as rtems
except ImportError:
    print('error: no rtems_waf git submodule')
    import sys
    sys.exit(1)

def init(ctx):
    rtems.init(ctx, version=rtems_version, long_commands=True)

def bsp_configure(conf, arch_bsp):
    # Add BSP specific configuration checks
    pass

def options(opt):
    rtems.options(opt)

    opt.add_option(
        '--enable-unit-test',
        action='store_true',
        default=False,
        help='Enable Unit Tests'
    )

    opt.add_option(
        '--enable-coverage',
        action='store_true',
        default=False,
        help='Enable gcov coverage instrumentation'
    )

    opt.add_option(
        '--test-group',
        action='store',
        default='',
        help='Run only the specified unit test group(s), comma-separated'
    )

    opt.add_option(
        '--test-filter',
        action='store',
        default='',
        help='Run only unit tests whose names contain this substring'
    )

    opt.add_option(
        '--list-tests',
        action='store_true',
        default=False,
        help='List registered unit tests instead of executing them'
    )

def configure(conf):
    rtems.configure(conf, bsp_configure=bsp_configure)

    if conf.options.enable_unit_test:
        conf.define('ENABLE_UNIT_TEST', True)
        conf.msg('Checking for Unit Test', 'Enabled')
    else:
        conf.msg('Checking for Unit Test', 'Disabled')

    if conf.options.enable_coverage:
        conf.define('ENABLE_COVERAGE', True)
        conf.msg('Checking for Coverage', 'Enabled')
    else:
        conf.msg('Checking for Coverage', 'Disabled')

    if conf.options.test_group:
        conf.define('RTFS_CONFIG_TEST_GROUP', conf.options.test_group)
        conf.msg('Checking for Test Group Filter', conf.options.test_group)
    else:
        conf.msg('Checking for Test Group Filter', 'Disabled')

    if conf.options.test_filter:
        conf.define('RTFS_CONFIG_TEST_FILTER', conf.options.test_filter)
        conf.msg('Checking for Test Name Filter', conf.options.test_filter)
    else:
        conf.msg('Checking for Test Name Filter', 'Disabled')

    if conf.options.list_tests:
        conf.define('RTFS_CONFIG_LIST_TESTS', 1)
        conf.msg('Checking for Test Listing Mode', 'Enabled')
    else:
        conf.msg('Checking for Test Listing Mode', 'Disabled')

    conf.write_config_header('rtfs_config.h')

def build(bld):
    rtems.build(bld)

    all_sources = bld.path.ant_glob('src/**/*.c', excl='**/test_*.c **/*_test.c') + bld.path.ant_glob('third_party/**/*.c')
    cflags = ['-g']
    linkflags = []
    libs = []

    # 这个做法太丑陋了，但是目前没找到合适的解法。conf 级别的配置和变量无法同步到 build 级别，我不知道为什么。
    with open('build/rtfs_config.h') as f:
        text = f.read()
    if '#define ENABLE_UNIT_TEST 1' in text:
        all_sources += bld.path.ant_glob('test/**/*.c')
        cflags.append('-DUNITY_SUPPORT_64')
    if '#define ENABLE_COVERAGE 1' in text:
        cflags += [
            '--coverage',
            '-fprofile-info-section=.rtemsroset.gcov_info.content',
            '-fprofile-update=atomic',
            '-DRTEMS_GCOV_COVERAGE'
        ]
        libs += ['rtemstest', 'gcov']

    include_paths = [
        bld.path.find_dir('build').abspath(),
        bld.path.find_dir('src').abspath(),
        bld.path.find_dir('test').abspath(),
        bld.path.find_dir('third_party').abspath()
    ]

    bld(features = 'c cprogram',
        target = 'main.exe',
        cflags = cflags,
        linkflags = linkflags,
        lib = libs,
        includes = include_paths,
        source = all_sources
    )
