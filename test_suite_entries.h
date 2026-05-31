#ifndef TEST_SUITE_ENTRIES_H
#define TEST_SUITE_ENTRIES_H

#define SPSC_TEST_SUITE_TABLE(X) \
    X("buffer_pool", run_tst_buffer_pool_api_paranoid) \
    X("fifo", run_tst_fifo_api_paranoid) \
    X("fifo_view", run_tst_fifo_view_api_paranoid) \
    X("pool", run_tst_pool_api_paranoid) \
    X("pool_view", run_tst_pool_view_api_paranoid) \
    X("latest", run_tst_latest_api_paranoid) \
    X("chunk", run_tst_chunk_api_paranoid) \
    X("queue", run_tst_queue_api_paranoid) \
    X("typed_pool", run_tst_typed_pool_api_paranoid)

#endif // TEST_SUITE_ENTRIES_H
