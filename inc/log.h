#ifndef __DEBUG_H
#define __DEBUG_H

#if defined(__cplusplus)
extern "C" {
#endif
    //detailed logs will record incoming and defered logs.
    //so the program will shorten the length of each item's log.
#define DETAILED_LOGS 0

    /* event types */
    enum
    {
        DIRCOUNTER_SYS,
    };

    //log level
#define LOG_EMERG   0   /* system in unusable */
#define LOG_ALERT   1   /* action must be taken immediately */
#define LOG_CRIT    2   /* critical conditions */
#define LOG_ERR     3   /* error conditions */
#define LOG_WARN    4   /* warning conditions */
#define LOG_NOTICE  5   /* normal but significant condition (default) */
#define LOG_INFO    6   /* informational */
#define LOG_DEBUG   7   /* debug messages */

    typedef struct log_conf
    {
        char *cfg_key;
        int cfg_shift;
    } log_conf;

    extern volatile int log_fd_invalid;
    extern log_conf log_cfg[10];

#define DIRCOUNTER_DEBUG_STARTED    \
    module_in_debugging(&logd_module)

#define debug_sys(level, fmt, arg...) \
    ({ persist_log(level, "[%s:%d][%s]"fmt, __FUNCTION__, __LINE__, log_cfg[level].cfg_key, ##arg);})

    int debug_init(void);
    void up_loglevel();
    void down_loglevel();
    extern void init_persist_log(void);
    int persist_log(int level, const char *format, ...);

#if defined(__cplusplus)
}
#endif

#endif
