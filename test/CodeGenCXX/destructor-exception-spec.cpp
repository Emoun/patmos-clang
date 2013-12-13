// RUN: %clang_cc1 -cxx-abi itanium -emit-llvm-only %s -std=c++11
// RUN: %clang_cc1 -cxx-abi itanium -emit-llvm-only -fno-use-cxa-atexit %s -std=c++11
// RUN: %clang_cc1 -cxx-abi microsoft -fno-rtti -emit-llvm-only %s -std=c++11

// PR13479: don't crash with -fno-exceptions.
namespace {
  struct SchedulePostRATDList {
    virtual ~SchedulePostRATDList();
  };

  SchedulePostRATDList::~SchedulePostRATDList() {}

  SchedulePostRATDList Scheduler;
}
