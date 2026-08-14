#ifndef DMW_VISIBILITY_CONTROL_HPP_
#define DMW_VISIBILITY_CONTROL_HPP_

#if defined _WIN32 || defined __CYGWIN__
#ifdef __GNUC__
#define DMW_EXPORT __attribute__((dllexport))
#define DMW_IMPORT __attribute__((dllimport))
#else
#define DMW_EXPORT __declspec(dllexport)
#define DMW_IMPORT __declspec(dllimport)
#endif
#ifdef DMW_BUILDING_LIBRARY
#define DMW_PUBLIC DMW_EXPORT
#else
#define DMW_PUBLIC DMW_IMPORT
#endif
#define DMW_LOCAL
#else
#define DMW_EXPORT __attribute__((visibility("default")))
#define DMW_IMPORT
#if __GNUC__ >= 4
#define DMW_PUBLIC __attribute__((visibility("default")))
#define DMW_LOCAL __attribute__((visibility("hidden")))
#else
#define DMW_PUBLIC
#define DMW_LOCAL
#endif
#endif

#endif  // DMW_VISIBILITY_CONTROL_HPP_
