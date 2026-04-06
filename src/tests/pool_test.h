#ifndef POOL_TEST_H_
#define POOL_TEST_H_

#include "test_config.hpp"

#if SPSC_TESTS_WITH_QT
int run_tst_pool_api_paranoid(int argc, char** argv);
void run_tst_pool_api_paranoid(bool verbose);
#endif

#endif /* POOL_TEST_H_ */
