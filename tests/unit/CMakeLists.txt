ExternalProject_Add(
        googletest
        PREFIX "${CMAKE_BINARY_DIR}/googletest"
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG main
        INSTALL_DIR "${CMAKE_BINARY_DIR}"
        CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}"
        STEP_TARGETS mkdir download update build configure install
)

# googletest-mkdir googletest-download googletest-update googletest-configure googletest-build
ADD_DEPENDENCIES( googletest-download googletest-mkdir )
ADD_DEPENDENCIES( googletest-update googletest-download )
ADD_DEPENDENCIES( googletest-configure googletest-update )
ADD_DEPENDENCIES( googletest-build googletest-configure )
ADD_DEPENDENCIES( googletest-install googletest-build )
ADD_DEPENDENCIES( googletest googletest-build )

INCLUDE_DIRECTORIES( ${CMAKE_BINARY_DIR}/include )
INCLUDE_DIRECTORIES( ${CMAKE_BINARY_DIR}/include )

ADD_SUBDIRECTORY( mockups )