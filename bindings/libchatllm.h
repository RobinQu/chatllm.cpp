#pragma once

#ifdef _WIN32
    #define API_CALL __stdcall
    #ifndef _WIN64
        #error unsupported target OS
    #endif
#elif __linux__
    #define API_CALL
    #if (!defined __x86_64__) && (!defined __aarch64__)
        #error unsupported target OS
    #endif
#else
    #define API_CALL
    #warning OS not supported, maybe
#endif

#ifndef DLL_DECL
#define DLL_DECL
#endif

#ifdef __cplusplus
extern "C"
{
#endif

enum PrintType
{
    PRINT_CHAT_CHUNK        = 0,
    // below items share the same value with BaseStreamer::TextType
    PRINTLN_META            = 1,    // print a whole line: general information
    PRINTLN_ERROR           = 2,    // print a whole line: error message
    PRINTLN_REF             = 3,    // print a whole line: reference
    PRINTLN_REWRITTEN_QUERY = 4,    // print a whole line: rewritten query
    PRINTLN_HISTORY_USER    = 5,    // print a whole line: user input history
    PRINTLN_HISTORY_AI      = 6,    // print a whole line: AI output history
    PRINTLN_TOOL_CALLING    = 7,    // print a whole line: tool calling (supported by only a few models)
};

typedef void (*f_chatllm_print)(void *user_data, int print_type, const char *utf8_str);
typedef void (*f_chatllm_end)(void *user_data);

struct chatllm_obj;

/**
 * Usage:
 *
 * ```c
 * obj = create(callback functions);
 * append_param(obj, ...);
 * // ...
 * app_param(obj, ...);
 *
 * start(obj);
 * while (true)
 * {
 *     user_input(obj, ...);
 * }
 * ```
*/

/**
 * @brief create ChatLLM object
 *
 * @return                  the object
 */
DLL_DECL struct chatllm_obj * API_CALL chatllm_create(void);

/**
 * @brief append a command line option
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          a command line option
 */
DLL_DECL void API_CALL chatllm_append_param(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief start
 *
 * @param[in] obj               model object
 * @param[in] f_print           callback function for printing
 * @param[in] f_end             callback function when model generation ends
 * @param[in] user_data         user data provided to callback functions
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_start(struct chatllm_obj *obj, f_chatllm_print f_print, f_chatllm_end f_end, void *user_data);

/**
 * @brief set max number of generated tokens in a new round of conversation
 *
 * @param[in] obj               model object
 * @param[in] gen_max_tokens    -1 for as many as possible
 */
DLL_DECL void API_CALL chatllm_set_gen_max_tokens(struct chatllm_obj *obj, int gen_max_tokens);

/**
 * @brief restart (i.e. discard history)
 *
 * * When a session has been loaded, the model is restarted to the point that the session is loaded;
 *
 *      Note: this would not work if `--extending` is not `none` or the model uses SWA.
 *
 * * Otherwise, it is restarted from the very beginning.
 *
 * @param[in] obj               model object
 */
DLL_DECL void API_CALL chatllm_restart(struct chatllm_obj *obj);

/**
 * @brief user input
 *
 * This function is synchronized, i.e. it returns after model generation ends and `f_end` is called.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          user input
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_user_input(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief tool input
 *
 * - If this function is called before `chatllm_user_input` returns, this is asynchronized,
 * - If this function is called after `chatllm_user_input` returns, this is equivalent to
 *   `chatllm_user_input`.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          user input
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_tool_input(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief abort generation
 *
 * This function is asynchronized, i.e. it returns immediately.
 *
 * @param[in] obj               model object
 */
DLL_DECL void API_CALL chatllm_abort_generation(struct chatllm_obj *obj);

/**
 * @brief show timing statistics
 *
 * Result is sent to `f_print`.
 *
 * @param[in] obj               model object
 */
DLL_DECL void API_CALL chatllm_show_statistics(struct chatllm_obj *obj);

/**
 * @brief save current session on demand
 *
 * Note: Call this from the same thread of `chatllm_user_input()`.
 *
 * If chat history is empty, then system prompt is evaluated and saved.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          file full name
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_save_session(struct chatllm_obj *obj, const char *utf8_str);

/**
 * @brief load a session on demand
 *
 * Note: Call this from the same thread of `chatllm_user_input()`.
 *
 * @param[in] obj               model object
 * @param[in] utf8_str          file full name
 * @return                      0 if succeeded
 */
DLL_DECL int API_CALL chatllm_load_session(struct chatllm_obj *obj, const char *utf8_str);

#ifdef __cplusplus
}
#endif