#define MOZ_UNIFIED_BUILD
#include "jit/arm/BaselineCompiler-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/BaselineCompiler-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/BaselineCompiler-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/BaselineIC-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/BaselineIC-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/BaselineIC-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/CodeGenerator-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/CodeGenerator-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/CodeGenerator-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/Lowering-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/Lowering-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/Lowering-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/MacroAssembler-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/MacroAssembler-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/MacroAssembler-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/MoveEmitter-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/MoveEmitter-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/MoveEmitter-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif