/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The build id of plat_selftest: the commit of the source tree it was built from, followed by
 * "-dirty-" and a hash of the uncommitted changes when tracked files differed from that
 * commit, or "unknown" without git. It is computed on every build (cmake/FcnBuildId.cmake), so
 * it is never older than the binary.
 * The self-test prints it in its JSON reports; bench/platform_timing.py records it and refuses
 * a result whose binary was not built from the tree the result names.
 */
#ifndef FCN_BUILD_ID_H
#define FCN_BUILD_ID_H

extern const char fcn_build_id[];

#endif /* FCN_BUILD_ID_H */
