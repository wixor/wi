#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#ifndef compile_time_assert

#define __cta2(x,line) static int __cta_ ## line [ !!(x) - 1 ] __attribute__((unused))
#define __cta(x,line) __cta2(x,line)
#define compile_time_assert(x) __cta(x,__LINE__)

#endif

