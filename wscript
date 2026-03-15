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

def configure(conf):
    rtems.configure(conf, bsp_configure=bsp_configure)

    if conf.options.enable_unit_test:
        conf.define('ENABLE_UNIT_TEST', True)
        conf.msg('Checking for Unit Test', 'Enabled')
    else:
        conf.msg('Checking for Unit Test', 'Disabled')

    conf.write_config_header('rtfs_config.h')

def build(bld):
    rtems.build(bld)

    all_sources = bld.path.ant_glob('src/**/*.c', excl='**/test_*.c **/*_test.c') + bld.path.ant_glob('third_party/**/*.c')

    # 这个做法太丑陋了，但是目前没找到合适的解法。conf 级别的配置和变量无法同步到 build 级别，我不知道为什么。
    with open('build/rtfs_config.h') as f:
        text = f.read()
    if '#define ENABLE_UNIT_TEST 1' in text:
        all_sources += bld.path.ant_glob('test/**/*.c')

    include_paths = [
        bld.path.find_dir('build').abspath(),
        bld.path.find_dir('src').abspath(),
        bld.path.find_dir('test').abspath(),
        bld.path.find_dir('third_party').abspath()
    ]

    bld(features = 'c cprogram',
        target = 'main.exe',
        cflags = '-g',
        includes = include_paths,
        source = all_sources
    )
