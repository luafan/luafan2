/*
 * platform.h — LuaFan v2 centralized platform detection & compatibility shims.
 * All #ifdef platform branches in v2 route through the FAN_PLATFORM_* / FAN_HAS_*
 * macros defined here, so per-platform code stays in one place.
 */
#ifndef FAN2_PLATFORM_H
#define FAN2_PLATFORM_H

/* ---- Platform detection --------------------------------------------------- */
#if defined(__APPLE__)
#  include <TargetConditionals.h>
#  if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#    define FAN_PLATFORM_IOS 1
#  else
#    define FAN_PLATFORM_MACOS 1
#  endif
#elif defined(__ANDROID__)
#  define FAN_PLATFORM_ANDROID 1
#elif defined(__linux__)
#  define FAN_PLATFORM_LINUX 1
#elif defined(_WIN32)
#  define FAN_PLATFORM_WINDOWS 1
#endif

/* ---- Thread-local storage keyword ----------------------------------------- */
#if defined(_MSC_VER)
#  define FAN_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define FAN_THREAD_LOCAL __thread
#else
#  define FAN_THREAD_LOCAL _Thread_local
#endif

/* ---- CPU affinity capability ---------------------------------------------- */
#if defined(FAN_PLATFORM_LINUX) || defined(FAN_PLATFORM_ANDROID)
#  define FAN_HAS_CPU_AFFINITY 1
#elif defined(FAN_PLATFORM_MACOS)
#  define FAN_HAS_CPU_AFFINITY 1
#else
#  define FAN_HAS_CPU_AFFINITY 0
#endif

/* ---- Feature switches (may be overridden by the build system) -------------- */
#ifndef FAN_WITH_OPENSSL
#  define FAN_WITH_OPENSSL 0
#endif
#ifndef FAN_WITH_CURL
#  define FAN_WITH_CURL 0
#endif
#ifndef FAN_WITH_MARIADB
#  define FAN_WITH_MARIADB 0
#endif
#ifndef FAN_WITH_WORKER
#  define FAN_WITH_WORKER 0
#endif

/* ---- Branch prediction hints ---------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#  define FAN_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define FAN_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define FAN_LIKELY(x)   (x)
#  define FAN_UNLIKELY(x) (x)
#endif

#endif /* FAN2_PLATFORM_H */
