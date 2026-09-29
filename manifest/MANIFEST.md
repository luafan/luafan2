# Manifest

Index of luafan2 v2 capabilities.  Each entry links to a detail file
under `manifest/` describing the feature, its milestone, and its
current test coverage.  Manifest entries reflect real shipped features,
not planning notes.

## Delivery + operations
- [Dual-form delivery: ./fan + fan.so](fan-so-module.md) — M15 thin
  executable that dlopen()s the fan.so shared module for dependency
  isolation (RTLD_LOCAL), while keeping v2's lua_State/loop ownership.
- [CI and release Docker images](ci-and-release-image.md) — M14.F
  GitHub Actions workflows + ubuntu / alpine runtime images published
  to Docker Hub.

## Application framework
- [Webase](webase.md) — M12.2 web application framework ported from
  v1's tmp/webase/, with LRU + mapping + service registry.

## Test coverage
- [Coverage — latest run](coverage-latest.md) — most recent
  C + Lua line coverage numbers with target thresholds.
- [Coverage plumbing](coverage.md) — how the three test modes
  (normal / --asan / --coverage) work; luacov `.luacov` layout;
  gcov / lcov invocation flow.
