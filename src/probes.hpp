#pragma once

#if defined(__linux__) && __has_include(<sys/sdt.h>)
#include <sys/sdt.h>
#else
#ifndef DTRACE_PROBE
#define DTRACE_PROBE(provider, name) ((void)0)
#endif
#ifndef STAP_PROBE
#define STAP_PROBE(provider, name) ((void)0)
#endif
#endif
