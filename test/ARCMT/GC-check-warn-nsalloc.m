// RUN: %clang_cc1 -arcmt-check -Werror -no-ns-alloc-error -triple x86_64-apple-darwin10 -fobjc-gc-only %s
// RUN: %clang_cc1 -arcmt-check -Wno-error=arcmt-ns-alloc -triple x86_64-apple-darwin10 -fobjc-gc-only %s 2>&1 | grep 'warning: \[rewriter\] call returns pointer to GC managed memory'
// RUN: %clang_cc1 -arcmt-check -Werror -no-ns-alloc-error -triple x86_64-apple-darwin10 -fobjc-gc-only -x objective-c++ %s
// TODO: Investigate VerifyDiagnosticConsumer failures on these tests when using -verify.
// rdar://10532541

typedef unsigned NSUInteger;
void *__strong NSAllocateCollectable(NSUInteger size, NSUInteger options);

void test1() {
  NSAllocateCollectable(100, 0);
}
