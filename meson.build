project('composite', 'c',
    default_options: ['buildtype=release', 'warning_level=2', 'b_lto=true', 'b_ndebug=if-release', 'c_std=c99'],
    license: 'GPL-3.0-or-later',
    license_files: 'COPYING',
    meson_version: '>=1.2.3',
    version: '1',
)

py = import('python').find_installation(pure: false)

incdir = include_directories(
    run_command(py, '-c', 'import vapoursynth as vs; print(vs.get_include())', check: true).stdout().strip(),
)

m_dep = meson.get_compiler('c').find_library('m', required: false)

core = static_library('composite_core', files('src/subcarrier.c', 'src/encode.c'),
    gnu_symbol_visibility: 'hidden',
    pic: true,
)

shared_module('composite',
    files('src/composite.c'),
    dependencies: m_dep,
    gnu_symbol_visibility: 'hidden',
    include_directories: incdir,
    install: true,
    install_dir: py.get_install_dir() / 'vapoursynth/plugins',
    link_with: core,
    name_prefix: '',
)

test('subcarrier', executable('test_subcarrier',
    files('test/test_subcarrier.c'),
    dependencies: m_dep,
    include_directories: include_directories('src'),
    link_with: core,
))

test('encode', executable('test_encode',
    files('test/test_encode.c'),
    dependencies: m_dep,
    include_directories: include_directories('src'),
    link_with: core,
))
