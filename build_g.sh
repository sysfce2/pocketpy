SRC=$(find src/ -name "*.cpp")

FLAGS="-std=c++17 -O2 -stdlib=libc++ -Iinclude -frtti -Wfatal-errors -g"

clang++ $FLAGS -o main src2/main.cpp $SRC
