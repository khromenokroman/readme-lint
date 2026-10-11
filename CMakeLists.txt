cmake_minimum_required(VERSION 3.25.1)
project(readme-lint
        VERSION 0.4.1.0
        LANGUAGES CXX
        HOMEPAGE_URL "https://github.com/khromenokroman/readme-lint")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Собирать тесты" ON)

find_package(fmt REQUIRED)

include(GNUInstallDirs)

configure_file(Version.in Version @ONLY)

add_library(${PROJECT_NAME}-core STATIC
        src/diagnostic.cpp
        src/markdown.cpp
        src/options.cpp
        src/repo.cpp
        src/rule_dbus.cpp
        src/rule_deps.cpp
        src/rule_names.cpp
)
target_include_directories(${PROJECT_NAME}-core PUBLIC src)
target_compile_definitions(${PROJECT_NAME}-core PUBLIC README_LINT_VERSION="${PROJECT_VERSION}")
target_link_libraries(${PROJECT_NAME}-core PUBLIC fmt::fmt-header-only)

add_executable(${PROJECT_NAME}
        src/main.cpp
)
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}-core)

if (BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(${PROJECT_NAME}-tests
            tests/diagnostic_test.cpp
            tests/markdown_test.cpp
            tests/options_test.cpp
            tests/repo_test.cpp
            tests/rule_dbus_test.cpp
            tests/rule_deps_test.cpp
            tests/rule_names_test.cpp
    )
    target_link_libraries(${PROJECT_NAME}-tests PRIVATE ${PROJECT_NAME}-core GTest::gtest_main GTest::gmock)
    include(GoogleTest)
    gtest_discover_tests(${PROJECT_NAME}-tests)
endif ()

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES debian/copyright
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "khromenokroman")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "khromenokroman <roma55592@yandex.ru>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Checks that README.md of a C++ project matches its code")
set(CPACK_PACKAGE_DESCRIPTION "Finds statements in README.md that contradict the project:\nmissing files and identifiers, outdated dependency versions,\nwrong D-Bus signatures and command line options.")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")

set(CPACK_GENERATOR "DEB")
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

include(CPack)
