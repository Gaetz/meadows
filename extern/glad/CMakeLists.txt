# glad2-generated loader (gl:core=4.6 + GL_ARB_bindless_texture), committed so
# the build needs no Python. To regenerate, from the repo root:
#   python -m glad --api='gl:core=4.6' --extensions=GL_ARB_bindless_texture \
#       --out-path=extern/glad --reproducible c
add_library(glad STATIC src/gl.c)
target_include_directories(glad SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
