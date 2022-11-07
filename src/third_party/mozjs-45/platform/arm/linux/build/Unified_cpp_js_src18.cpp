#define MOZ_UNIFIED_BUILD
#include "jit/TypedObjectPrediction.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/TypedObjectPrediction.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/TypedObjectPrediction.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/VMFunctions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/VMFunctions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/VMFunctions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/ValueNumbering.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/ValueNumbering.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/ValueNumbering.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/Architecture-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/Architecture-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/Architecture-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/Assembler-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/Assembler-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/Assembler-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "jit/arm/Bailouts-arm.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "jit/arm/Bailouts-arm.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "jit/arm/Bailouts-arm.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif