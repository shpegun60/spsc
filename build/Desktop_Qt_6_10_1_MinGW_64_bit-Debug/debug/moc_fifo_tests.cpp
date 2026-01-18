/****************************************************************************
** Meta object code from reading C++ file 'fifo_tests.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.10.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../fifo_tests.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'fifo_tests.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.10.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN9FifoTestsE_t {};
} // unnamed namespace

template <> constexpr inline auto FifoTests::qt_create_metaobjectdata<qt_meta_tag_ZN9FifoTestsE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "FifoTests",
        "test_static_plain_int",
        "",
        "test_static_atomic_int",
        "test_static_volatile_int",
        "test_static_cached_int",
        "test_static_complex_types",
        "test_dynamic_plain_int",
        "test_dynamic_resize",
        "test_bulk_operations",
        "test_iterators",
        "test_snapshots",
        "test_guards",
        "test_move_semantics",
        "test_swap_semantics",
        "test_copy_semantics",
        "test_paranoid_single_thread",
        "bench_single_thread_plain",
        "bench_single_thread_volatile",
        "bench_single_thread_atomic",
        "bench_single_thread_cached",
        "bench_threaded_atomic",
        "bench_threaded_cached"
    };

    QtMocHelpers::UintData qt_methods {
        // Slot 'test_static_plain_int'
        QtMocHelpers::SlotData<void()>(1, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_static_atomic_int'
        QtMocHelpers::SlotData<void()>(3, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_static_volatile_int'
        QtMocHelpers::SlotData<void()>(4, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_static_cached_int'
        QtMocHelpers::SlotData<void()>(5, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_static_complex_types'
        QtMocHelpers::SlotData<void()>(6, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_dynamic_plain_int'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_dynamic_resize'
        QtMocHelpers::SlotData<void()>(8, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_bulk_operations'
        QtMocHelpers::SlotData<void()>(9, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_iterators'
        QtMocHelpers::SlotData<void()>(10, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_snapshots'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_guards'
        QtMocHelpers::SlotData<void()>(12, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_move_semantics'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_swap_semantics'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_copy_semantics'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'test_paranoid_single_thread'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'bench_single_thread_plain'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'bench_single_thread_volatile'
        QtMocHelpers::SlotData<void()>(18, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'bench_single_thread_atomic'
        QtMocHelpers::SlotData<void()>(19, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'bench_single_thread_cached'
        QtMocHelpers::SlotData<void()>(20, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'bench_threaded_atomic'
        QtMocHelpers::SlotData<void()>(21, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'bench_threaded_cached'
        QtMocHelpers::SlotData<void()>(22, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<FifoTests, qt_meta_tag_ZN9FifoTestsE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject FifoTests::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9FifoTestsE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9FifoTestsE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN9FifoTestsE_t>.metaTypes,
    nullptr
} };

void FifoTests::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<FifoTests *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->test_static_plain_int(); break;
        case 1: _t->test_static_atomic_int(); break;
        case 2: _t->test_static_volatile_int(); break;
        case 3: _t->test_static_cached_int(); break;
        case 4: _t->test_static_complex_types(); break;
        case 5: _t->test_dynamic_plain_int(); break;
        case 6: _t->test_dynamic_resize(); break;
        case 7: _t->test_bulk_operations(); break;
        case 8: _t->test_iterators(); break;
        case 9: _t->test_snapshots(); break;
        case 10: _t->test_guards(); break;
        case 11: _t->test_move_semantics(); break;
        case 12: _t->test_swap_semantics(); break;
        case 13: _t->test_copy_semantics(); break;
        case 14: _t->test_paranoid_single_thread(); break;
        case 15: _t->bench_single_thread_plain(); break;
        case 16: _t->bench_single_thread_volatile(); break;
        case 17: _t->bench_single_thread_atomic(); break;
        case 18: _t->bench_single_thread_cached(); break;
        case 19: _t->bench_threaded_atomic(); break;
        case 20: _t->bench_threaded_cached(); break;
        default: ;
        }
    }
    (void)_a;
}

const QMetaObject *FifoTests::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *FifoTests::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9FifoTestsE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int FifoTests::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 21)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 21;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 21)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 21;
    }
    return _id;
}
QT_WARNING_POP
