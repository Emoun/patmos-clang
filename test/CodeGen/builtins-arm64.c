// RUN: %clang_cc1 -triple arm64-apple-ios -O3 -emit-llvm -o - %s | FileCheck %s

void f0(void *a, void *b) {
	__clear_cache(a,b);
// CHECK: call {{.*}} @__clear_cache
}

// CHECK: call {{.*}} @llvm.aarch64.rbit.i32(i32 %a)
void rbit(unsigned a) {
  __builtin_arm_rbit(a);
}

// CHECK: call {{.*}} @llvm.aarch64.rbit.i64(i64 %a)
void rbit64(unsigned long long a) {
  __builtin_arm_rbit64(a);
}
