#pragma once
#include <stddef.h>

#ifdef __cplusplus
#define EXTERN_C_BEGIN extern "C" {
#define EXTERN_C_END }
#else
#define EXTERN_C_BEGIN
#define EXTERN_C_END
#endif

#define SZ_OK 0
#define SZ_ERROR_DATA 1
#define SZ_ERROR_MEM 2
#define SZ_ERROR_UNSUPPORTED 4
#define SZ_ERROR_INPUT_EOF 6
#define SZ_ERROR_FAIL 11
#define RINOK(x) { const int result_ = (x); if (result_ != SZ_OK) return result_; }
#if defined(_MSC_VER) && defined(_M_IX86)
#define Z7_FASTCALL __fastcall
#else
#define Z7_FASTCALL
#endif

typedef int SRes;
typedef int BoolInt;
#define True 1
#define False 0
typedef signed int Int32;
typedef unsigned char Byte;
typedef unsigned short UInt16;
typedef unsigned int UInt32;
typedef size_t SizeT;

typedef struct ISzAlloc ISzAlloc;
typedef const ISzAlloc *ISzAllocPtr;
struct ISzAlloc {
  void *(*Alloc)(ISzAllocPtr p, size_t size);
  void (*Free)(ISzAllocPtr p, void *address);
};
#define ISzAlloc_Alloc(p, size) (p)->Alloc(p, size)
#define ISzAlloc_Free(p, address) (p)->Free(p, address)
