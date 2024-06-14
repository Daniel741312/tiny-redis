#ifndef LOG_H
#define LOG_H

#ifdef DBG
    #define LOG(...) \
            printf("[%d] %s: ", __LINE__, __func__), \
            printf(__VA_ARGS__), \
            printf("\n")

#else
    #define LOG(...) \
        ;
#endif

#endif