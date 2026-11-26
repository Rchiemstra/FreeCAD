if(NOT EXISTS "${WRAPPER_DIR}/coin.py")
    message(FATAL_ERROR "Pivy SWIG wrapper is missing: ${WRAPPER_DIR}/coin.py")
endif()
file(MAKE_DIRECTORY "${DEST_DIR}")
# Remove obsolete Python files without touching this configuration's extension.
file(GLOB_RECURSE old_python LIST_DIRECTORIES false
    "${DEST_DIR}/*.py" "${DEST_DIR}/*.pyi" "${DEST_DIR}/py.typed")
if(old_python)
    file(REMOVE ${old_python})
endif()
file(COPY "${SOURCE_DIR}/" DESTINATION "${DEST_DIR}"
    FILES_MATCHING PATTERN "*.py" PATTERN "*.pyi" PATTERN "py.typed")
file(COPY "${WRAPPER_DIR}/coin.py" DESTINATION "${DEST_DIR}")
