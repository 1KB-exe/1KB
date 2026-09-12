# LZMA decoder provenance

`LzmaDec.c` and `LzmaDec.h` are the public-domain LZMA decoder from the
7-Zip source tree (Igor Pavlov), revision dated 2023-04-02:
https://github.com/ip7z/7zip/tree/master/C

`LzmaTypes.h` is the minimal set of portable SDK types and allocator definitions
needed to compile that decoder. The unrelated remainder of `7zTypes.h` is not vendored.
