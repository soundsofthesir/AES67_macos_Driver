# Writes Version.h with the version from VERSION.txt and the git commit.
# Runs on every build, so the commit is current; configure_file leaves the
# file untouched when nothing changed, so nothing recompiles needlessly.
#
# Inputs: SOURCE_DIR, OUTPUT, VERSION, BUILD

set(COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE head
        RESULT_VARIABLE result OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(result EQUAL 0)
        set(COMMIT "${head}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE changes ERROR_QUIET)
        if(NOT changes STREQUAL "")
            string(APPEND COMMIT "-modified")
        endif()
    endif()
endif()

configure_file("${SOURCE_DIR}/Shared/Version.h.in" "${OUTPUT}" @ONLY)
