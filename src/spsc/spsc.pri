INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

HEADERS +=                                  \
    $$PWD/array_fifo.hpp \
    $$PWD/base/SPSCbase.hpp                 \
    $$PWD/base/spsc_alloc.hpp               \
    $$PWD/base/spsc_cacheline.hpp           \
    $$PWD/base/spsc_capacity_ctrl.hpp       \
    $$PWD/base/spsc_config.hpp              \
    $$PWD/base/spsc_counter.hpp             \
    $$PWD/base/spsc_object.hpp              \
    $$PWD/base/spsc_policy.hpp              \
    $$PWD/base/spsc_regions.hpp \
    $$PWD/base/spsc_snapshot.hpp            \
    $$PWD/base/spsc_tools.hpp \
    $$PWD/chunk.hpp \
    $$PWD/chunk_fifo.hpp \
    $$PWD/fifo.hpp \
    $$PWD/fifo_view.hpp \
    $$PWD/latest.hpp \
    $$PWD/pool.hpp \
    $$PWD/pool_view.hpp \
    $$PWD/queue.hpp \
    $$PWD/typed_pool.hpp

SOURCES += \
