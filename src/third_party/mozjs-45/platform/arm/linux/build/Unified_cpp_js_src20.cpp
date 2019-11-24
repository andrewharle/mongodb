#define MOZ_UNIFIED_BUILD
#include "jit/arm/SharedIC-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/SharedIC-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/SharedIC-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/Trampoline-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/Trampoline-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/Trampoline-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/disasm/Constants-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/disasm/Constants-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/disasm/Constants-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/disasm/Disasm-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/disasm/Disasm-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/disasm/Disasm-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/shared/BaselineCompiler-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/shared/BaselineCompiler-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/shared/BaselineCompiler-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/shared/CodeGenerator-shared.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/shared/CodeGenerator-shared.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/shared/CodeGenerator-shared.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif