#
# Main component makefile.
#
# This Makefile can be left empty. By default, it will take the sources in the 
# src/ directory, compile them and link them into lib(subdirectory_name).a 
# in the build directory. This behaviour is entirely configurable,
# please read the ESP-IDF documents if you need to do this.
#


# 显式指定源文件目录为 src
COMPONENT_SRCDIRS := src

# 显式指定头文件目录为 include
COMPONENT_ADD_INCLUDEDIRS := include