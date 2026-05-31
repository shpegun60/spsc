#ifndef SPSC_TEST_BUILD_CONFIG_HPP
#define SPSC_TEST_BUILD_CONFIG_HPP

#include "test_config.hpp"

#if !defined(SPSC_ASSERT) && !defined(NDEBUG)
#  include <cstdlib>
#  define SPSC_ASSERT(expr) do { if (!(expr)) { std::abort(); } } while (0)
#endif

#include "../spsc/base/SPSCbase.hpp"

#if SPSC_TESTS_WITH_QT

#include <QDebug>
#include <QString>
#include <QStringView>

#include <climits>
#include <limits>

#include "../../basic_types.h"

#ifndef SPSC_ENABLE_SHADOW_INDICES
#  define SPSC_ENABLE_SHADOW_INDICES 1
#endif

#ifndef SPSC_SHADOW_ALLOW_32BIT
#  define SPSC_SHADOW_ALLOW_32BIT 0
#endif

#ifndef SPSC_SHADOW_REFRESH_HEURISTIC
#  define SPSC_SHADOW_REFRESH_HEURISTIC 0
#endif

#ifndef SPSC_TEST_EXPECTED_VARIANT_NAME
#  define SPSC_TEST_EXPECTED_VARIANT_NAME "unspecified"
#endif

#ifndef SPSC_TEST_ACTUAL_TARGET_NAME
#  define SPSC_TEST_ACTUAL_TARGET_NAME "unspecified"
#endif

#ifndef SPSC_TEST_EXPECTED_ENABLE_SHADOW_INDICES
#  define SPSC_TEST_EXPECTED_ENABLE_SHADOW_INDICES SPSC_ENABLE_SHADOW_INDICES
#endif

#ifndef SPSC_TEST_EXPECTED_SHADOW_ALLOW_32BIT
#  define SPSC_TEST_EXPECTED_SHADOW_ALLOW_32BIT SPSC_SHADOW_ALLOW_32BIT
#endif

#ifndef SPSC_TEST_EXPECTED_SHADOW_REFRESH_HEURISTIC
#  define SPSC_TEST_EXPECTED_SHADOW_REFRESH_HEURISTIC SPSC_SHADOW_REFRESH_HEURISTIC
#endif

namespace spsc_test {

static constexpr int kActualEnableShadowIndices = SPSC_ENABLE_SHADOW_INDICES;
static constexpr int kActualShadowAllow32Bit = SPSC_SHADOW_ALLOW_32BIT;
static constexpr int kActualShadowRefreshHeuristic = SPSC_SHADOW_REFRESH_HEURISTIC;

static constexpr int kExpectedEnableShadowIndices = SPSC_TEST_EXPECTED_ENABLE_SHADOW_INDICES;
static constexpr int kExpectedShadowAllow32Bit = SPSC_TEST_EXPECTED_SHADOW_ALLOW_32BIT;
static constexpr int kExpectedShadowRefreshHeuristic = SPSC_TEST_EXPECTED_SHADOW_REFRESH_HEURISTIC;

static constexpr int kRegBits = std::numeric_limits<reg>::digits;

static constexpr bool shadow_enabled_for_atomic_backend(int enable_shadow_indices,
                                                        int shadow_allow_32bit) noexcept
{
    return (enable_shadow_indices != 0) &&
           ((kRegBits >= 64) || (shadow_allow_32bit != 0));
}

static constexpr bool kActualAtomicShadow =
    ::spsc::detail::rb_use_shadow_v<::spsc::policy::A<>>;

static constexpr bool kActualCachedShadow =
    ::spsc::detail::rb_use_shadow_v<::spsc::policy::CA<>>;

static constexpr bool kExpectedAtomicShadow =
    shadow_enabled_for_atomic_backend(kExpectedEnableShadowIndices, kExpectedShadowAllow32Bit);

static constexpr bool kExpectedCachedShadow =
    shadow_enabled_for_atomic_backend(kExpectedEnableShadowIndices, kExpectedShadowAllow32Bit);

static inline QString expected_variant_name()
{
    return QString::fromUtf8(SPSC_TEST_EXPECTED_VARIANT_NAME);
}

static inline QString actual_variant_name()
{
    const QString target = QString::fromUtf8(SPSC_TEST_ACTUAL_TARGET_NAME);
    const QString prefix = QStringLiteral("spsc_test_");
    return target.startsWith(prefix) ? target.mid(prefix.size()) : target;
}

static inline QString format_build_config_summary(QStringView suite_name,
                                                  QStringView variant_name,
                                                  int enable_shadow_indices,
                                                  int shadow_allow_32bit,
                                                  int shadow_refresh_heuristic,
                                                  bool atomic_shadow,
                                                  bool cached_shadow)
{
    QString summary = QStringLiteral(
        "suite=%1 variant=%2 macros{enable_shadow=%3 allow_32bit=%4 refresh_heuristic=%5} "
        "effective{reg_bits=%6 atomic_A_shadow=%7 cached_CA_shadow=%8}");
    summary = summary.arg(suite_name.toString());
    summary = summary.arg(variant_name.toString());
    summary = summary.arg(enable_shadow_indices);
    summary = summary.arg(shadow_allow_32bit);
    summary = summary.arg(shadow_refresh_heuristic);
    summary = summary.arg(kRegBits);
    summary = summary.arg(atomic_shadow ? 1 : 0);
    summary = summary.arg(cached_shadow ? 1 : 0);
    return summary;
}

static inline QString actual_build_config_summary(QStringView suite_name)
{
    return format_build_config_summary(suite_name,
                                       actual_variant_name(),
                                       kActualEnableShadowIndices,
                                       kActualShadowAllow32Bit,
                                       kActualShadowRefreshHeuristic,
                                       kActualAtomicShadow,
                                       kActualCachedShadow);
}

static inline QString expected_build_config_summary(QStringView suite_name)
{
    return format_build_config_summary(suite_name,
                                       expected_variant_name(),
                                       kExpectedEnableShadowIndices,
                                       kExpectedShadowAllow32Bit,
                                       kExpectedShadowRefreshHeuristic,
                                       kExpectedAtomicShadow,
                                       kExpectedCachedShadow);
}

static inline void log_build_config(QStringView suite_name)
{
    qInfo().noquote() << "[spsc-test-config]" << actual_build_config_summary(suite_name);
}

} // namespace spsc_test

#define SPSC_TEST_VERIFY_BUILD_CONFIG(SUITE_NAME)                                              \
    do {                                                                                       \
        const QString spsc_test_actual_cfg =                                                   \
            ::spsc_test::actual_build_config_summary(QString::fromUtf8(SUITE_NAME));           \
        const QString spsc_test_expected_cfg =                                                 \
            ::spsc_test::expected_build_config_summary(QString::fromUtf8(SUITE_NAME));         \
        qInfo().noquote() << "[spsc-test-config]" << spsc_test_actual_cfg;                     \
        QCOMPARE(spsc_test_actual_cfg, spsc_test_expected_cfg);                                \
    } while (false)

#endif // SPSC_TESTS_WITH_QT

#endif // SPSC_TEST_BUILD_CONFIG_HPP
