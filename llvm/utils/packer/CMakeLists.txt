cmake_minimum_required(VERSION 3.20.0)
project(packer NONE)

# Install pack.py with execute permission
install(
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/pack.py
        DESTINATION ${CMAKE_INSTALL_PREFIX}/share/llvm/packer
        PERMISSIONS OWNER_EXECUTE OWNER_READ GROUP_READ WORLD_READ
)

# Install packer sources and vendored Python modules. System python3 is used at
# runtime, so bundled interpreter payloads are intentionally omitted.
install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
        DESTINATION ${CMAKE_INSTALL_PREFIX}/share/llvm/packer/
        PATTERN "cpython" EXCLUDE
        PATTERN "cpython-GLIBC-2.35" EXCLUDE
        PATTERN "perfs" EXCLUDE
)
