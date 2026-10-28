# writes build_info.h with the commit the tree is at right now; run on every build (cmake -P), the file only
# changes when the commit or the dirty state does, so main.cpp is not rebuilt for nothing
# usage: cmake -DREPO=<repo> -DOUT=<build_info.h> -P build_info.cmake
set(commit "unknown")
find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD WORKING_DIRECTORY ${REPO}
                    OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
    if(NOT failed AND head)
        set(commit ${head})
        # tracked files changed since the commit: the log says so
        execute_process(COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD WORKING_DIRECTORY ${REPO}
                        RESULT_VARIABLE changed ERROR_QUIET)
        if(changed)
            set(commit "${commit}-dirty")
        endif()
    endif()
endif()
set(text "#define BLACK_COMMIT \"${commit}\"\n")
set(old "")
if(EXISTS ${OUT})
    file(READ ${OUT} old)
endif()
if(NOT old STREQUAL text)
    file(WRITE ${OUT} "${text}")
endif()
