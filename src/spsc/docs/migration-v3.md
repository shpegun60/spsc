# Migrating from v2 to v3

## The Deliberate Breaking Change

In v2.1, an ordinary bare policy-based container used the historical plain
default:

```cpp
spsc::fifo<Message, 128> // v2.1: policy::P
```

In v3, the same spelling uses the normal portable SPSC default when no legacy
override macro is defined:

```cpp
spsc::fifo<Message, 128> // v3: policy::FA<>
```

This is an intentional major-version change, not a silent safety tweak. The
two forms are different C++ types and may differ in policy type, metadata
layout, `sizeof`, `alignof`, generated code, atomic requirements,
and ABI.

The shipped P-to-FA transition does not by itself change the policy-derived
default allocator: both policies request allocator alignment `1`. Cache-aligned
policies continue to select the aligned allocator path explicitly.

With the default shadow gates, an atomic FA policy enables shadow metadata when
`reg` is at least 64 bits. On a usual 32-bit STM32 build shadows stay disabled
unless `SPSC_SHADOW_ALLOW_32BIT=1` is selected. That often makes the host and MCU
layout impact different, but neither layout should be treated as ABI-compatible
with v2 without recompiling every translation unit.

## Preferred Migration Before Upgrading

On v2.1.x, make the intended contract explicit first:

```diff
// Preserve the historical plain contract and its policy-derived layout.
- using Queue = spsc::fifo<Message, 128>;
+ using Queue = spsc::local_fifo<Message, 128>;

// State a real portable one-producer/one-consumer handoff explicitly.
- using Queue = spsc::fifo<Message, 128>;
+ using Queue = spsc::concurrent_fifo<Message, 128>;
```

Use `cache_aligned_fifo<Message, 128>` when the same concurrent contract needs
cache-line isolation for a measured target. The same `local_*`,
`concurrent_*`, and `cache_aligned_*` scheme applies to all policy-driven SPSC
transport containers and views.

After either explicit migration, the v3 default-policy change cannot alter that
declaration.

## Legacy Build Override

`SPSC_DEFAULT_POLICY_ATOMIC` is retained only as a compatibility input. The
library does not define it when absent:

| Build configuration | `spsc::policy::default_policy` |
| --- | --- |
| no macro | `FA<>` |
| `-DSPSC_DEFAULT_POLICY_ATOMIC=0` | `P` |
| `-DSPSC_DEFAULT_POLICY_ATOMIC=1` | `A<>` |

That preserves the meaning of existing explicit v2 configurations. In
particular, `=1` continues to mean strict RMW `A<>`; it is not remapped to
`FA<>`. New code should not use this macro to select a policy.

If the legacy override is used, give it one identical build-wide value in every
translation unit. Mixing absent, `0`, and `1` configurations can make the same
bare source spelling denote different C++ types and is an ABI/ODR violation.

## Other Default-Dependent Spellings

`policy::CacheAligned<>` defaults its `Base` argument to `default_policy`.
Consequently its mapping follows the same table:

| Build configuration | `policy::CacheAligned<>` |
| --- | --- |
| no macro | `CFA<>` |
| `-DSPSC_DEFAULT_POLICY_ATOMIC=0` | `CP` |
| `-DSPSC_DEFAULT_POLICY_ATOMIC=1` | `CA<>` |

Use `CP`, `CFA<>`, `CA<>`, or `CacheAligned<ExplicitPolicy>` when the base must
not depend on a build-wide default.

Policy-less CTAD also follows `default_policy`. For example,
`fifo_view(storage, capacity)` and `pool_view(slots, capacity, buffer_size)` now
deduce FA-based view types in the normal v3 configuration. Pass an explicit
policy constructor argument when using the advanced CTAD form, or name a
semantic view alias when the type must remain independent of the build override.

The headers also contain `SPSCbase` and `cap::CapacityCtrl` implementation
templates whose omitted policy follows the same mapping. They are not supported
application extension APIs; application code should use concrete containers,
views, semantic aliases, and explicit public policies instead.

## Storage Helpers

`buffer_pool` is not an SPSC endpoint, but it has a `Policy` template parameter
and therefore also follows `default_policy` when omitted. If its physical
storage layout matters across the upgrade, spell the policy explicitly:

```cpp
using PlainStorage = spsc::buffer_pool<std::byte, 100, 8, spsc::policy::P>;
using DmaStorage = spsc::buffer_pool<std::byte, 100, 8, spsc::policy::CP>;
```

The readable `static_buffer_pool`, `fixed_buffer_pool`,
`fixed_count_buffer_pool`, and `dynamic_buffer_pool` aliases also inherit
`default_policy` when their policy argument is omitted.

Keep the ownership transport separate, for example with
`concurrent_pool_view` or `cache_aligned_pool_view` as appropriate.

For the shipped P and FA policies, `buffer_pool` keeps the same physical
alignment, span, `sizeof`, `alignof`, and default allocator. Its policy template
argument still makes the v3 bare form a different C++ type, and selecting FA can
require a toolchain whose `reg` atomic is always lock-free even though
`buffer_pool` itself has no producer/consumer indices.

## New v3 Declarations

For new concurrent SPSC code, these are equivalent under the normal v3
configuration:

```cpp
using BareQueue = spsc::fifo<Message, 128>;
using ExplicitQueue = spsc::concurrent_fifo<Message, 128>;
```

Choose `local_*` when plain counters and external synchronization are truly
intentional. Use the full `Container<..., Policy, ...>` form only when an
advanced policy such as `A<>`, `CA<>`, `V`, `VV`, `CP`, `AA<>`, or `CAA<>` is
the actual contract.
