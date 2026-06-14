project('ish', 'c',
    default_options: ['default_library=static', 'c_std=gnu11', 'warning_level=2'])
cc = meson.get_compiler('c')

if cc.get_id() == 'clang'
    add_project_arguments('-Wimplicit-fallthrough', '-Wtautological-constant-in-range-compare', language: 'c')
endif

if get_option('b_sanitize').split(',').contains('undefined')
    add_project_arguments('-fno-sanitize=alignment', language: 'c')
endif

log_on = get_option('log').split()
log_off = get_option('nolog').split()
foreach channel : log_on + log_off
    if log_on.contains(channel)
        add_project_arguments('-DDEBUG_' + channel + '=1', language: 'c')
    else
        add_project_arguments('-DDEBUG_' + channel + '=0', language: 'c')
    endif
endforeach
add_project_arguments('-DLOG_HANDLER_' + get_option('log_handler').to_upper() + '=1', language: 'c')
add_project_arguments('-DENGINE_' + get_option('engine').to_upper() + '=1', language: 'c')

if get_option('no_crlf')
    add_project_arguments('-DNO_CRLF', language: 'c')
endif

add_project_arguments('-Wno-switch', language: 'c')

includes = [include_directories('.')]

threads = dependency('threads')
librt = cc.find_library('rt', required: false)
libm = cc.find_library('m', required: false)
libdl = cc.find_library('dl', required: false)
sqlite3 = cc.find_library('sqlite3')
dependencies = [librt, libm, libdl, threads, sqlite3]

subdir('vdso') # ish depends on the vdso

offsets = custom_target('offsets',
    output: 'cpu-offsets.h', input: 'jit/offsets.c', depfile: 'cpu-offsets.h.d',
    command: [find_program('tools/staticdefine.sh'), '@OUTDIR@/compile_commands.json', '@INPUT@', '@OUTPUT@', '@DEPFILE@'])

# Generate the embedded /AOK/tests table (consumed by fs/aok.c) from the curated
# fs/aok-tests.manifest, so the regression sources under tests/manual/ ship in
# /AOK/tests automatically. Editing a listed test, or adding a manifest line,
# propagates on the next build. The manifest is read at configure time so every
# listed source is a proper build dependency.
fs_mod = import('fs')
python = find_program('python3')
aok_test_srcs = []
foreach line : fs_mod.read('fs/aok-tests.manifest').strip().split('\n')
    entry = line.strip()
    if entry != '' and not entry.startswith('#')
        aok_test_srcs += files('tests/manual' / entry.split()[0])
    endif
endforeach
aok_generated_tests = custom_target('aok_generated_tests',
    output: 'aok_generated_tests.inc',
    input: [files('tools/gen-aokfs.py', 'fs/aok-tests.manifest')] + aok_test_srcs,
    command: [python, '@INPUT0@', '@INPUT1@',
              meson.current_source_dir() / 'tests' / 'manual', '@OUTPUT@'])

emu_src = [
    'emu/tlb.c',
    'emu/fpu.c',
    'emu/vec.c',
    'emu/mmx.c',
    'emu/float80.c',
    'emu/amd64_interp.c',
    'jit/jit.c',
    'jit/gen.c',
    'jit/helpers.c',
]
jit_cpu_family = get_option('jit_cpu_family')
if jit_cpu_family == ''
    jit_cpu_family = host_machine.cpu_family()
endif
gadgets = 'jit/gadgets-' + jit_cpu_family
emu_src += [
    gadgets+'/entry.S',
    gadgets+'/memory.S',
    gadgets+'/control.S',
    gadgets+'/math.S',
    gadgets+'/bits.S',
    gadgets+'/string.S',
    gadgets+'/misc.S',
    offsets,
]

libish_emu = library('ish_emu', emu_src, include_directories: includes)

libfakefs = library('fakefs',
    ['fs/fake-db.c', 'fs/fake-migrate.c', 'fs/fake-rebuild.c'],
    include_directories: includes,
    dependencies: sqlite3)

subdir('deps')

if get_option('kernel') == 'ish'
    src = [
        'kernel/init.c',
        'kernel/errno.c',

        'kernel/calls.c',
        'kernel/user.c',
        'kernel/vdso.c', vdso,
        'kernel/task.c',
        'kernel/group.c',
        'kernel/log.c',

        'kernel/fork.c',
        'kernel/exec.c',
        'kernel/exit.c',
        'kernel/time.c',
        'kernel/mmap.c',
        'kernel/uname.c',
        'kernel/tls.c',
        'kernel/futex.c',
        'kernel/getset.c',
        'kernel/signal.c',
        'kernel/resource.c',
        'kernel/random.c',
        'kernel/misc.c',
        'kernel/eventfd.c',
        'kernel/inotify.c',
        'kernel/ipc.c',
        'kernel/memfd.c',
        'kernel/ptrace.c',

        'kernel/fs.c',
        'kernel/fs_info.c',
        'fs/aok.c', aok_generated_tests,
        'fs/mount.c',
        'fs/fd.c',
        'fs/inode.c',
        'fs/stat.c',
        'fs/dir.c',
        'fs/generic.c',
        'fs/path.c',
        'fs/net_route.c',
        'fs/real.c',
        'fs/fake.c',

        'fs/proc.c',
        'fs/proc/entry.c',
        'fs/proc/ish.c',
        'fs/proc/net.c',
        'fs/proc/sys.c',
        'fs/proc/root.c',
        'fs/proc/pid.c',

        'fs/dyndev.c',

        'fs/adhoc.c',
        'fs/sock.c',
        'fs/pipe.c',
        'fs/sockrestart.c',
        'fs/lock.c',

        'fs/dev.c',
        'fs/mem.c',
        'fs/tty.c',
        'fs/tty-real.c',
        'fs/pty.c',
        'fs/tmp.c',

        'fs/poll.c',
        'kernel/poll.c',
        'kernel/epoll.c',

        'util/timer.c',
        'util/ro_locks.c',
        'util/rw_locks.c',
        'util/sync.c',
        'util/fifo.c',

        'emu/memory.c',
        'platform/standalone.c',

        'platform/' + host_machine.system() + '.c',
    ]

    libish = library('ish', src,
        include_directories: includes)
    ish = declare_dependency(
        link_with: [libish, libish_emu, libfakefs],
        dependencies: dependencies,
        include_directories: includes)

    if not meson.is_cross_build()
        executable('ish', ['main.c'], dependencies: ish)
    endif

elif get_option('kernel') == 'linux'
    modules = static_library('linux_modules', [
        'linux/main.c',
        'linux/fakefs.c',
    ], dependencies: [linux_headers, sqlite3])
    executable('ish', [build_linux],
        link_with: [libfakefs],
        dependencies: [
            liblinux,
            libm,
            libdl,
            threads,
            declare_dependency(link_whole: modules),
        ])
endif

subdir('tools')

gdb_scripts = ['ish-gdb.gdb']
foreach script : gdb_scripts
    custom_target(script,
        output: script, input: script,
        command: ['ln', '-sf', '@INPUT@', '@OUTPUT@'],
        build_by_default: true)
endforeach

if not meson.is_cross_build()
    # test for floating point library
    float80_test = executable('float80_test', ['emu/float80.c', 'emu/float80-test.c'], dependencies: [libm])
    test('float80', float80_test)
endif

e2e_test = find_program('tests/e2e/e2e.bash')
test('e2e', e2e_test, args: ['-y'], timeout: 180)
