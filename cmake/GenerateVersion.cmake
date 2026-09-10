# Resolves git version metadata and writes the version header.
# Run via `cmake -P`. Inputs passed with -D (GIT_EXECUTABLE, SRC_DIR, IN, OUT).
# BME_GIT_TAG = exact tag if HEAD is a cut release, else the current branch (source build).
set(BME_GIT_TAG "unknown")
set(BME_GIT_HASH "unknown")
set(BME_GIT_URL "https://github.com/angelfor3v3r/bme")

if (GIT_EXECUTABLE AND EXISTS "${SRC_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE BME_EXACT_TAG
        RESULT_VARIABLE BME_EXACT_TAG_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if (BME_EXACT_TAG_RESULT EQUAL 0 AND BME_EXACT_TAG)
        set(BME_GIT_TAG "${BME_EXACT_TAG}")
    else ()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
            WORKING_DIRECTORY "${SRC_DIR}"
            OUTPUT_VARIABLE BME_GIT_BRANCH
            RESULT_VARIABLE BME_GIT_BRANCH_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)

        if (BME_GIT_BRANCH_RESULT EQUAL 0 AND BME_GIT_BRANCH)
            set(BME_GIT_TAG "${BME_GIT_BRANCH}")
        endif ()
    endif ()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE BME_RESOLVED_GIT_HASH
        RESULT_VARIABLE BME_GIT_HASH_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if (BME_GIT_HASH_RESULT EQUAL 0 AND BME_RESOLVED_GIT_HASH)
        set(BME_GIT_HASH "${BME_RESOLVED_GIT_HASH}")
    endif ()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" remote get-url origin
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE BME_GIT_REMOTE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    # Normalize the `origin` remote to a browseable URL (keeps the hardcoded default if there's no remote).
    if (BME_GIT_REMOTE)
        # ssh -> https.
        string(REGEX REPLACE "^git@([^:]+):" "https://\\1/" BME_GIT_URL "${BME_GIT_REMOTE}")

        # Drop `.git`.
        string(REGEX REPLACE "\\.git$" "" BME_GIT_URL "${BME_GIT_URL}")
    endif ()
endif ()

# Only rewrites OUT when the contents change, so dependents recompile only when the version moves.
configure_file("${IN}" "${OUT}" @ONLY)
