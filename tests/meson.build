envs = environment()
# Enable debug messages and abort on critical warnings
envs.set('G_DEBUG', 'fatal-criticals')
envs.set('G_MESSAGES_DEBUG', 'all')

# Setup paths
accountsservice_build_dir = meson.build_root() / 'src' / 'libaccountsservice'
envs.prepend('LD_LIBRARY_PATH', accountsservice_build_dir)

python3_test_modules_found = false
if get_option('introspection')
    python3 = find_program('python3') # No meson without it!
    python3_test_modules_found = true
    python3_test_modules = [
        'gi',
        'dbusmock',
    ]

    foreach module : python3_test_modules
        if run_command(python3, '-c', 'import @0@'.format(module)).returncode() != 0
            python3_test_modules_found = false
            warning('Python3 module \'' + module + '\' needed by test suite not found')
            break
        endif
    endforeach
endif

tests = [
    'libaccountsservice',
]

if python3_test_modules_found
    envs.prepend('GI_TYPELIB_PATH', accountsservice_build_dir)
    unittest_inspector = find_program('unittest_inspector.py')

    foreach test_name: tests
        test_file = files('test-' + test_name + '.py')
        base_args = test_file
        suite = ['accounts-service']

        r = run_command(unittest_inspector, test_file)
        unit_tests = r.stdout().strip().split('\n')

        if r.returncode() == 0 and unit_tests.length() > 0
            suite += test_name
        else
            unit_tests = [test_name]
        endif

        foreach ut: unit_tests
            ut_suite = suite
            ut_args = base_args
            if unit_tests.length() > 1
                ut_args += ut
                ut_suite += ut.split('.')[0]
            endif
            test(ut,
                python3,
                args: ut_args,
                suite: ut_suite,
                depends: as_gir_gen_sources,
                env: envs,
            )
        endforeach
    endforeach
else
    warning('Skipping AccuntsService tests as introspection bindings are missing')

    foreach test_name: tests
        test(test_name, find_program('sh'), args: ['-c', 'exit 77'])
    endforeach
endif
