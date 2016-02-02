#define MOZ_UNIFIED_BUILD
#include "FloatingPoint.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "FloatingPoint.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "FloatingPoint.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "HashFunctions.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "HashFunctions.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "HashFunctions.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "JSONWriter.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "JSONWriter.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "JSONWriter.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "Poison.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "Poison.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "Poison.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "SHA1.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "SHA1.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "SHA1.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "TaggedAnonymousMemory.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "TaggedAnonymousMemory.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "TaggedAnonymousMemory.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/bignum-dtoa.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/bignum-dtoa.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/bignum-dtoa.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/bignum.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/bignum.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/bignum.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/cached-powers.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/cached-powers.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/cached-powers.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/diy-fp.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/diy-fp.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/diy-fp.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/double-conversion.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/double-conversion.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/double-conversion.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/fast-dtoa.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/fast-dtoa.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/fast-dtoa.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/fixed-dtoa.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/fixed-dtoa.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/fixed-dtoa.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "double-conversion/strtod.cc"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "double-conversion/strtod.cc uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "double-conversion/strtod.cc defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif
#include "unused.cpp"
#ifdef PL_ARENA_CONST_ALIGN_MASK
#error "unused.cpp uses PL_ARENA_CONST_ALIGN_MASK, so it cannot be built in unified mode."
#undef PL_ARENA_CONST_ALIGN_MASK
#endif
#ifdef INITGUID
#error "unused.cpp defines INITGUID, so it cannot be built in unified mode."
#undef INITGUID
#endif