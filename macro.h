/*
 * macro.h
 *
 * Macros for working with design patterns
 * Version: 2.1
 */

#ifndef CHAOS_PP_MACRO_H_
#define CHAOS_PP_MACRO_H_

// Helper macro for creating default constructor
#define _DEFAULT_CONSTRUCTOR() = default

// Helper macro for deleting copy/move operations
#define _DELETE_COPY_MOVE(ClassName)         			\
    ClassName(ClassName&) = delete;          			\
    ClassName(const ClassName&) = delete;          		\
    ClassName& operator=(ClassName&&) = delete;			\
    ClassName& operator=(const ClassName&) = delete; 	\
    ClassName(ClassName&&) = delete;               		\
    static_assert(true, "Require semicolon after macro")

/**
 * @brief Creates a singleton class (Singleton)
 * @param ClassName - Class name
 * @param ... Additional specifiers for the constructor
 *
 * Example of use:
 * class MyClass {
 *     SINGLETON(MyClass, = default);
 * };
 */
#define SINGLETON(ClassName, ...)                  			\
private:                                           			\
    ClassName() __VA_ARGS__;                       			\
    ~ClassName() = default;                        			\
    _DELETE_COPY_MOVE(ClassName);            				\
public:                                            			\
    static ClassName& instance() noexcept {        			\
        static ClassName instance;                 			\
        return instance;                           			\
    }                                              			\
private:													\
	static_assert(true, "Require semicolon after macro")

/**
* @brief Creates a static class without instances
* @param ClassName -  Class name
*
* Usage example:
* STATIC_CLASS(Utility);
*/
#define STATIC_CLASS(ClassName)                    		\
private:												\
	ClassName() = delete;                          		\
    ~ClassName() = delete;                         		\
    _DELETE_COPY_MOVE(ClassName);            			\
    static_assert(true, "Require semicolon after macro")



#endif /* CHAOS_PP_MACRO_H_ */
