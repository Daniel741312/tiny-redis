#ifndef LOG_H
#define LOG_H

#ifdef DBG
    #define LOG(...) \
            printf("[%d] %s: ", __LINE__, __func__), \
            printf(__VA_ARGS__), \
            printf("\n")
        
    #define LOG_RED(...) \
            printf("\033[31m[%d] %s: ", __LINE__, __func__), \
            printf(__VA_ARGS__), \
            printf("\n\033[0m")

    #define LOG_GREEN(...) \
            printf("\033[32m[%d] %s: ", __LINE__, __func__), \
            printf(__VA_ARGS__), \
            printf("\n\033[0m")
            
    #define LOG_BLUE(...) \
            printf("\033[34m[%d] %s: ", __LINE__, __func__), \
            printf(__VA_ARGS__), \
            printf("\n\033[0m")


#else
    #define LOG(...) \
        ;
#endif

#endif