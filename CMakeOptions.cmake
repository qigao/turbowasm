set(CMAKE_COLOR_DIAGNOSTICS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# building the tests
option(ENABLE_TESTS "Enable the tests" OFF)

option(ENABLE_GIT_INFO "List git status" OFF)
option(ENABLE_SAMPLES "Enable sample projects" ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)


option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
