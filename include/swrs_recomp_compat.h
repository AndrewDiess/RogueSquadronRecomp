// Force-included into the RecompiledFuncs sources (see CMakeLists.txt).
// Fills gaps between the N64Recomp version that generated the code and the recomp.h
// bundled with this N64ModernRuntime version.
#ifndef SWRS_RECOMP_COMPAT_H
#define SWRS_RECOMP_COMPAT_H

// `lwu`: load word, zero-extended. Newer N64Recomp emits MEM_WU; this runtime's recomp.h
// only has MEM_W (sign-extended). Expanded where used, after recomp.h has defined MEM_W.
#ifndef MEM_WU
#define MEM_WU(offset, reg) ((uint64_t)(uint32_t)MEM_W(offset, reg))
#endif

#endif
