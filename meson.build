project('composite', 'c',
    default_options: ['buildtype=release', 'warning_level=2', 'b_lto=true', 'b_ndebug=if-release', 'c_std=c99'],
    license: 'GPL-3.0-or-later',
    license_files: 'COPYING',
    meson_version: '>=1.2.3',
    version: '1',
)

cc = meson.get_compiler('c')

# libm calls stay bit-exact without errno; keeping them errno-free lets
# the vectorizers treat lrintf and friends as pure and vectorize the
# quantization loops
add_project_arguments(cc.get_supported_arguments('-fno-math-errno'), language: 'c')

py = import('python').find_installation(pure: false)

incdir = include_directories(
    run_command(py, '-c', 'import vapoursynth as vs; print(vs.get_include())', check: true).stdout().strip(),
)

m_dep = cc.find_library('m', required: false)
fftw3f_dep = dependency('fftw3f')
threads_dep = dependency('threads')
core_deps = [m_dep, fftw3f_dep, threads_dep]

core_sources = files('src/subcarrier.c', 'src/encode.c', 'src/transform2d.c',
                     'src/transform3d.c', 'src/decode.c', 'src/cpu.c')

if host_machine.cpu_family() == 'x86_64'
    add_languages('nasm', native: false)
    add_project_arguments(
        '-DARCH_X86_64=1', '-DPIC', '-Dprivate_prefix=comp',
        '-I' + meson.current_source_dir() / 'x86/',
        language: 'nasm',
    )
    core_sources += files('x86/cpu.asm', 'x86/pal_demod.asm', 'x86/fir_row.asm',
                          'x86/split3d.asm', 'x86/lut_gain.asm')
endif

core = static_library('composite_core',
    core_sources,
    dependencies: core_deps,
    gnu_symbol_visibility: 'hidden',
    pic: true,
)

shared_module('composite',
    files('src/composite.c', 'src/lut_tables.c'),
    dependencies: core_deps,
    gnu_symbol_visibility: 'hidden',
    include_directories: incdir,
    install: true,
    install_dir: py.get_install_dir() / 'vapoursynth/plugins',
    link_with: core,
    name_prefix: '',
)

test('subcarrier', executable('test_subcarrier',
    files('test/test_subcarrier.c'),
    dependencies: core_deps,
    include_directories: include_directories('src'),
    link_with: core,
))

test('encode', executable('test_encode',
    files('test/test_encode.c'),
    dependencies: core_deps,
    include_directories: include_directories('src'),
    link_with: core,
))

test('transform2d', executable('test_transform2d',
    files('test/test_transform2d.c'),
    dependencies: core_deps + dependency('fftw3'),
    include_directories: include_directories('src'),
    link_with: core,
))

test('decode', executable('test_decode',
    files('test/test_decode.c'),
    dependencies: core_deps,
    include_directories: include_directories('src'),
    link_with: core,
))

checkasm_dep = dependency('checkasm', required: get_option('checkasm'), allow_fallback: true)
if checkasm_dep.found()
    checkasm_exe = executable('checkasm',
        files('test/checkasm/main.c', 'test/checkasm/pal_demod.c',
              'test/checkasm/fir_row.c', 'test/checkasm/split3d.c',
              'test/checkasm/lut_gain.c'),
        dependencies: [checkasm_dep] + core_deps,
        include_directories: include_directories('src'),
        link_with: core,
    )
    test('checkasm', checkasm_exe)
endif
