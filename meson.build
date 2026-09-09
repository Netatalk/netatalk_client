project(
    'netatalk-client',
    'c',
    version: '1.0.0',
    license: 'GPLv2',
    default_options: ['warning_level=3', 'werror=true', 'c_std=c11'],
    meson_version: '>=0.62',
)

cc = meson.get_compiler('c')
host_os = host_machine.system()
pkg = import('pkgconfig')
uname = find_program('uname', required: false)
uname_stdout = run_command(uname, '-a', check: false).stdout().strip()

install_prefix = get_option('prefix')
bindir = install_prefix / get_option('bindir')
libdir = install_prefix / get_option('libdir')
mandir = install_prefix / get_option('mandir')
datadir = install_prefix / get_option('datadir')
includedir = install_prefix / get_option('includedir')

# The Solaris runtime linker does not search /usr/local/lib by default. Keep
# installed binaries and libraries pointed at Meson's configured library dir.
install_runtime_path = host_os == 'sunos' ? libdir : ''

libsearch_dirs = []
include_dirs = []
cflags = [
    '-DNETATALK_CLIENT_VERSION="' + meson.project_version() + '"',
    '-DBINDIR="' + bindir + '"',
    '-D_FILE_OFFSET_BITS=64',
]

if 'afp' in get_option('enable-debugging')
    cflags += '-DDEBUG_AFP_LOOP'
endif

if 'dsi' in get_option('enable-debugging')
    cflags += '-DDEBUG_DSI'
endif

if host_os == 'linux'
    cflags += '-D_GNU_SOURCE'
elif host_os == 'sunos'
    # illumos/Solaris hides POSIX and system extension declarations (including
    # sigaction(), strnlen(), and strlcpy()) when compiling in strict C mode.
    cflags += '-D__EXTENSIONS__'
    if uname.found() and uname_stdout.to_lower().contains('omnios')
        libsearch_dirs += '/opt/local/lib'
        include_dirs += ['/opt/local/include']
    endif
elif host_os in ['dragonfly', 'freebsd', 'openbsd']
    libsearch_dirs += '/usr/local/lib'
    include_dirs += ['/usr/local/include']
    cflags += '-DLIBICONV_PLUG'
elif host_os == 'netbsd'
    libsearch_dirs += '/usr/pkg/lib'
    include_dirs += ['/usr/pkg/include']
endif

readline_dep = cc.find_library('readline', dirs: libsearch_dirs, required: false)
editline_dep = dependency('libedit', required: false)
pthread_dep = cc.find_library('pthread', dirs: libsearch_dirs, required: true)
libiconv_dep = cc.find_library('iconv', dirs: libsearch_dirs, required: false)
libbsd_dep = cc.find_library('bsd', dirs: libsearch_dirs, required: false)

iconv_check_deps = []
if libiconv_dep.found()
    iconv_check_deps += libiconv_dep
endif

# POSIX specifies char ** for iconv()'s input buffer, while Solaris and some
# other historical implementations declare it as const char **.
if cc.compiles(
    '''
    #include <stdlib.h>
    #include <iconv.h>
    extern size_t iconv(iconv_t cd, const char **inbuf, size_t *inbytesleft,
                        char **outbuf, size_t *outbytesleft);
    ''',
    args: cflags,
    dependencies: iconv_check_deps,
    include_directories: include_directories(include_dirs),
    name: 'iconv with const inbuf',
)
    cflags += '-DICONV_CONST=const'
endif

root_dependencies = []
cmdline_dependencies = []

libedit_readline_header = ''
if editline_dep.found()
    if cc.has_header('editline/readline.h', dependencies: editline_dep)
        libedit_readline_header = 'editline/readline.h'
    elif cc.has_header('readline.h', dependencies: editline_dep)
        libedit_readline_header = 'readline.h'
    endif
endif

if host_os == 'sunos'
    # illumos/Solaris provides socket(), getaddrinfo(), and related interfaces
    # in libsocket rather than libc.
    root_dependencies += cc.find_library('socket', required: true)
endif

if libedit_readline_header != ''
    cmdline_dependencies += editline_dep
    cflags += '-DHAVE_LIBEDIT'
    if libedit_readline_header == 'editline/readline.h'
        cflags += '-DHAVE_EDITLINE_READLINE_H'
    endif
elif readline_dep.found()
    cmdline_dependencies += readline_dep
    cflags += '-DHAVE_LIBREADLINE'
endif

if libbsd_dep.found()
    root_dependencies += libbsd_dep
    cflags += '-DHAVE_LIBBSD'
endif

if cc.has_function(
    'explicit_bzero',
    prefix: '#include <strings.h>',
    args: cflags,
)
    cflags += '-DHAVE_EXPLICIT_BZERO'
elif cc.has_function(
    'memset_explicit',
    prefix: '#include <string.h>',
    args: cflags,
)
    cflags += '-DHAVE_MEMSET_EXPLICIT'
endif

gcrypt_dep = dependency('libgcrypt', version: '>=1.2.3', required: false)
fuse_dep = dependency('fuse3', version: '>=3.0.0', required: false)
if not fuse_dep.found() or get_option('force-fuse-v2')
    fuse_dep = dependency('fuse', version: '>=2.9.0', required: false)
endif

with_afpcmd = readline_dep.found() or libedit_readline_header != ''
with_fuse = get_option('enable-fuse') and fuse_dep.found()
with_crypt = gcrypt_dep.found()

# Check for optional Zeroconf discovery support. DNS-SD is part of libSystem
# on macOS, while other implementations commonly provide libdns_sd.
zeroconf_option = get_option('zeroconf')
zeroconf_backend_option = get_option('zeroconf-backend')
avahi_dep = dependency('avahi-client', required: false)
dnssd_lib = cc.find_library('dns_sd', required: false)
dnssd_dependencies = []

if dnssd_lib.found()
    dnssd_dependencies += dnssd_lib
endif

have_dnssd = (
    cc.has_header('dns_sd.h')
    and cc.has_function(
        'DNSServiceBrowse',
        prefix: '#include <dns_sd.h>',
        dependencies: dnssd_dependencies,
    )
)

have_dnssd_getaddrinfo = (
    have_dnssd
    and cc.has_function(
        'DNSServiceGetAddrInfo',
        prefix: '#include <dns_sd.h>',
        dependencies: dnssd_dependencies,
    )
)

zeroconf_backend = 'stub'
zeroconf_dependencies = []
zeroconf_cargs = []

if not zeroconf_option.disabled()
    if zeroconf_backend_option == 'avahi'
        if avahi_dep.found()
            zeroconf_backend = 'avahi'
            zeroconf_dependencies += avahi_dep
        elif zeroconf_option.enabled()
            error(
                'Avahi Zeroconf backend requested but avahi-client was not found',
            )
        endif
    elif zeroconf_backend_option == 'dnssd'
        if have_dnssd
            zeroconf_backend = 'dnssd'
            zeroconf_dependencies += dnssd_dependencies
        elif zeroconf_option.enabled()
            error(
                'DNS-SD Zeroconf backend requested but dns_sd.h or DNSServiceBrowse was not found',
            )
        endif
    elif host_os == 'darwin'
        if have_dnssd
            zeroconf_backend = 'dnssd'
            zeroconf_dependencies += dnssd_dependencies
        elif avahi_dep.found()
            zeroconf_backend = 'avahi'
            zeroconf_dependencies += avahi_dep
        endif
    else
        if avahi_dep.found()
            zeroconf_backend = 'avahi'
            zeroconf_dependencies += avahi_dep
        elif have_dnssd
            zeroconf_backend = 'dnssd'
            zeroconf_dependencies += dnssd_dependencies
        endif
    endif
endif

with_zeroconf = zeroconf_backend != 'stub'

if zeroconf_backend == 'dnssd' and have_dnssd_getaddrinfo
    zeroconf_cargs += '-DHAVE_DNSSD_GETADDRINFO'
endif

if zeroconf_option.enabled() and not with_zeroconf
    error('Zeroconf discovery enabled but neither Avahi nor DNS-SD is available')
endif

# Check for extended attribute headers (platform-dependent)
if cc.has_header('sys/xattr.h')
    cflags += '-DHAVE_SYS_XATTR_H'
endif
if cc.has_header('sys/extattr.h')
    cflags += '-DHAVE_SYS_EXTATTR_H'
endif
if cc.has_header('attr/xattr.h')
    cflags += '-DHAVE_ATTR_XATTR_H'
endif

if with_fuse
    fuse_version = fuse_dep.version()
    if fuse_version.version_compare('>=3.0.0')
        cflags += [
            '-DHAVE_LIBFUSE',
            '-DFUSE_USE_VERSION=35',
        ]
    else
        cflags += [
            '-DHAVE_LIBFUSE',
            '-DFUSE_USE_VERSION=29',
        ]
    endif

    # Detect FUSE capability: new readdir API with fuse_readdir_flags
    # This tests if enum fuse_readdir_flags type exists (FUSE 3.3+)
    fuse_readdir_flags_check = cc.compiles(
        '''
        #define FUSE_USE_VERSION 35
        #include <fuse.h>
        enum fuse_readdir_flags flags;
    ''',
        dependencies: fuse_dep,
        name: 'FUSE enum fuse_readdir_flags',
    )

    # Detect FUSE capability: new function signatures with struct fuse_file_info *fi parameter
    # This tests if a function with the new signature can be defined and used
    fuse_file_info_ops_check = cc.compiles(
        '''
        #define FUSE_USE_VERSION 35
        #include <fuse.h>

        int chown_func(const char *path, uid_t uid, gid_t gid,
                       struct fuse_file_info *fi) { return 0; }
        int chmod_func(const char *path, mode_t mode,
                       struct fuse_file_info *fi) { return 0; }
        int truncate_func(const char *path, off_t offset,
                          struct fuse_file_info *fi) { return 0; }
    ''',
        dependencies: fuse_dep,
        name: 'FUSE new function signatures with file_info',
    )

    # Detect FUSE capability: new rename API with flags parameter
    fuse_rename_flags_check = cc.compiles(
        '''
        #define FUSE_USE_VERSION 35
        #include <fuse.h>
        int rename_func(const char *from, const char *to,
                        unsigned int flags) { return 0; }
    ''',
        dependencies: fuse_dep,
        name: 'FUSE rename with flags parameter',
    )

    # Detect FUSE capability: new utimens API with file_info parameter
    fuse_utimens_fi_check = cc.compiles(
        '''
        #define FUSE_USE_VERSION 35
        #include <fuse.h>
        int utimens_func(const char *path, const struct timespec tv[2],
                         struct fuse_file_info *fi) { return 0; }
    ''',
        dependencies: fuse_dep,
        name: 'FUSE utimens with file_info parameter',
    )

    # Define FUSE_NEW_API if we have the new API capabilities
    # We consider it "new API" if all the new signatures are supported
    if fuse_readdir_flags_check and fuse_file_info_ops_check and fuse_rename_flags_check and fuse_utimens_fi_check
        cflags += '-DFUSE_NEW_API=1'
    else
        cflags += '-DFUSE_NEW_API=0'
    endif
endif

incdir = include_directories(['.', 'include', include_dirs])

if with_crypt
    add_project_arguments('-DHAVE_LIBGCRYPT', language: 'c')
endif

subdir('common')
subdir('discovery')
subdir('lib')
subdir('daemon')

if with_fuse
    subdir('fuse')
endif

subdir('control')
subdir('cmdline')
subdir('test')

subdir('include')

if get_option('enable-docs')
    subdir('docs')

    install_data(
        ['AUTHORS', 'COPYING', 'NEWS.md', 'README.md'],
        install_dir: datadir / 'doc' / 'netatalk-client',
    )
endif

summary_info = {
    'Build directory': meson.current_build_dir(),
    'Source path': meson.current_source_dir(),
}
summary(summary_info, bool_yn: true, section: 'Build environment:')

summary_info = {
    'Installation prefix': install_prefix,
    'Executable directory': bindir,
    'Library directory': libdir,
    'Manual page directory': mandir,
}
summary(summary_info, bool_yn: true, section: 'Directories:')

summary_info = {
    'Host CPU': host_machine.cpu_family(),
    'Host endianness': build_machine.endian(),
    'C compiler': cc.get_id(),
    'Build type': get_option('buildtype'),
    'Shared or static libraries': get_option('default_library'),
}
summary(summary_info, bool_yn: true, section: 'Compilation:')

summary_info = {
    'AFP control client': true,
    'AFP command line client': with_afpcmd,
    'AFP FUSE client': with_fuse,
    'Encrypted UAMs': with_crypt,
    'Zeroconf discovery': with_zeroconf,
    'Zeroconf backend': zeroconf_backend,
}
summary(summary_info, bool_yn: true, section: 'Options:')
