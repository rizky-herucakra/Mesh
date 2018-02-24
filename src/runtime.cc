// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2017 University of Massachusetts, Amherst

#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include "runtime.h"

namespace mesh {

ATTRIBUTE_ALIGNED(CACHELINE_SIZE)
const unsigned char SizeMap::class_array_[kClassArraySize] = {
#include "size_classes.def"
};

ATTRIBUTE_ALIGNED(CACHELINE_SIZE)
const int32_t SizeMap::class_to_size_[kClassSizesMax] = {
    0,   16,  32,  48,  64,  80,  96,  112,  128,  160,  192,  224,   256,
    320, 384, 448, 512, 640, 768, 896, 1024, 2048, 4096, 8192, 16384,
};

// const internal::BinToken::Size internal::BinToken::Max = numeric_limits<uint32_t>::max();
// const internal::BinToken::Size internal::BinToken::MinFlags = numeric_limits<uint32_t>::max() - 4;

// const internal::BinToken::Size internal::BinToken::FlagFull = numeric_limits<uint32_t>::max() - 1;
// const internal::BinToken::Size internal::BinToken::FlagEmpty = numeric_limits<uint32_t>::max() - 2;
// const internal::BinToken::Size internal::BinToken::FlagNoOff = numeric_limits<uint32_t>::max();

__thread LocalHeap *Runtime::_localHeap;

STLAllocator<char, internal::Heap> internal::allocator{};

size_t internal::measurePssKiB() {
  size_t sz = 0;
  auto fd = open("/proc/self/smaps_rollup", O_RDONLY | O_CLOEXEC);
  if (unlikely(fd < 0)) {
    mesh::debug("measurePssKiB: no smaps_rollup");
    return 0;
  }

  static constexpr size_t BUF_LEN = 1024;
  char buf[BUF_LEN];
  memset(buf, 0, BUF_LEN);

  (void)read(fd, buf, BUF_LEN - 1);
  close(fd);

  auto start = strstr(buf, "\nPss: ");
  if (unlikely(start == nullptr)) {
    mesh::debug("measurePssKiB: no Pss");
    return 0;
  }

  return atoi(&start[6]);
}

int internal::copyFile(int dstFd, int srcFd, off_t off, size_t sz) {
#if defined(__APPLE__) || defined(__FreeBSD__)
#error FIXME: use off
  // fcopyfile works on FreeBSD and OS X 10.5+
  int result = fcopyfile(srcFd, dstFd, 0, COPYFILE_ALL);
#else
  d_assert(off >= 0);

  off_t newOff = lseek(dstFd, off, SEEK_SET);
  d_assert(newOff == off);

  errno = 0;
  // sendfile will work with non-socket output (i.e. regular file) on Linux 2.6.33+
  int result = sendfile(dstFd, srcFd, &off, sz);
#endif

  return result;
}

Runtime::Runtime() {
}

int Runtime::createThread(pthread_t *thread, const pthread_attr_t *attr, PthreadFn startRoutine, void *arg) {
  lock_guard<Runtime> lock(*this);

  if (mesh::real::pthread_create == nullptr)
    mesh::real::init();

  void *threadArgsBuf = mesh::internal::Heap().malloc(sizeof(StartThreadArgs));
  d_assert(threadArgsBuf != nullptr);
  StartThreadArgs *threadArgs = new (threadArgsBuf) StartThreadArgs(this, startRoutine, arg);

  return mesh::real::pthread_create(thread, attr, reinterpret_cast<PthreadFn>(startThread), threadArgs);
}

void *Runtime::startThread(StartThreadArgs *threadArgs) {
  d_assert(threadArgs != nullptr);

  Runtime *runtime = threadArgs->runtime;
  PthreadFn startRoutine = threadArgs->startRoutine;
  void *arg = threadArgs->arg;

  mesh::internal::Heap().free(threadArgs);
  threadArgs = nullptr;

  runtime->installSegfaultHandler();

  return startRoutine(arg);
}

void Runtime::createSignalFd() {
  sigset_t mask;

  sigemptyset(&mask);
  sigaddset(&mask, SIGDUMP);

  mesh::real::init();

  /* Block signals so that they aren't handled
     according to their default dispositions */

  auto result = mesh::real::sigprocmask(SIG_BLOCK, &mask, NULL);
  hard_assert(result == 0);

  _signalFd = signalfd(-1, &mask, 0);
  hard_assert(_signalFd >= 0);
}

void Runtime::startBgThread() {
  constexpr int MaxRetries = 20;

  pthread_t bgPthread;
  int retryCount = 0;
  int ret = 0;

  while ((ret = pthread_create(&bgPthread, nullptr, Runtime::bgThread, nullptr))) {
    retryCount++;
    sched_yield();

    if (retryCount % 10)
      debug("background thread creation failed, retrying.\n");

    if (retryCount >= MaxRetries) {
      debug("max retries exceded: couldn't create bg thread, exiting.\n");
      abort();
    }
  }
}

void *Runtime::bgThread(void *arg) {
  auto &rt = mesh::runtime();

  // debug("libmesh: background thread started\n");

  while (true) {
    struct signalfd_siginfo siginfo;

    ssize_t s = read(rt._signalFd, &siginfo, sizeof(struct signalfd_siginfo));
    if (s != sizeof(struct signalfd_siginfo)) {
      if (s >= 0) {
        debug("bad read size: %lld\n", s);
        abort();
      } else {
        // read returns -1 if the program gets a process-killing signal
        return nullptr;
      }
    }

    if (static_cast<int>(siginfo.ssi_signo) == SIGDUMP) {
      // debug("libmesh: background thread received SIGDUMP, starting dump\n");
      debug(">>>>>>>>>>\n");
      rt.heap().dumpStrings();
      // debug("<<<<<<<<<<\n");

      // debug("<<<<<<<<<<\n");
    } else {
      printf("Read unexpected signal\n");
    }
  }
}

void Runtime::lock() {
  _mutex.lock();
}

void Runtime::unlock() {
  _mutex.unlock();
}

void Runtime::allocLocalHeap() {
  d_assert(_localHeap == nullptr);

  void *buf = mesh::internal::Heap().malloc(sizeof(LocalHeap));
  if (buf == nullptr) {
    mesh::debug("mesh: unable to allocate LocalHeap, aborting.\n");
    abort();
  }
  _localHeap = new (buf) LocalHeap(&mesh::runtime().heap());
}

int Runtime::epollWait(int __epfd, struct epoll_event *__events, int __maxevents, int __timeout) {
  if (unlikely(mesh::real::epoll_wait == nullptr))
    mesh::real::init();

  _heap.maybeMesh();

  return mesh::real::epoll_wait(__epfd, __events, __maxevents, __timeout);
}

int Runtime::epollPwait(int __epfd, struct epoll_event *__events, int __maxevents, int __timeout,
                        const __sigset_t *__ss) {
  if (unlikely(mesh::real::epoll_pwait == nullptr))
    mesh::real::init();

  _heap.maybeMesh();

  return mesh::real::epoll_pwait(__epfd, __events, __maxevents, __timeout, __ss);
}

static struct sigaction sigsegv_action;
static mutex sigsegv_lock;

int Runtime::sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
  if (unlikely(mesh::real::sigaction == nullptr))
    mesh::real::init();

  if (signum != SIGSEGV)
    return mesh::real::sigaction(signum, act, oldact);

  // if a user is trying to install a segfault handler, record that
  // here to proxy to later.
  lock_guard<mutex> lock(sigsegv_lock);

  if (oldact)
    memcpy(oldact, &sigsegv_action, sizeof(sigsegv_action));

  if (act == nullptr)
    memset(&sigsegv_action, 0, sizeof(sigsegv_action));
  else
    memcpy(&sigsegv_action, act, sizeof(sigsegv_action));

  return 0;
}

int Runtime::sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  if (unlikely(mesh::real::sigprocmask == nullptr))
    mesh::real::init();

  lock_guard<mutex> lock(sigsegv_lock);

  debug("TODO: ensure we never mask SIGSEGV\n");

  return mesh::real::sigprocmask(how, set, oldset);
}

void Runtime::segfaultHandler(int sig, siginfo_t *siginfo, void *context) {
  // okToProceed is a barrier that ensures any in-proress meshing has
  // completed, and the reason for the fault was 'just' a meshing
  if (siginfo->si_code == SEGV_ACCERR && runtime().heap().okToProceed(siginfo->si_addr)) {
    // debug("TODO: trapped access violation from meshing, log stat\n");
    return;
  }

  debug("TODO: check for + call program's handler\n");
  abort();
}

void Runtime::installSegfaultHandler() {
  struct sigaction action;
  struct sigaction old_action;

  memset(&action, 0, sizeof(action));
  memset(&old_action, 0, sizeof(old_action));

  action.sa_sigaction = segfaultHandler;
  action.sa_flags = SA_SIGINFO | SA_NODEFER;
  auto err = mesh::real::sigaction(SIGSEGV, &action, &old_action);
  hard_assert(err == 0);

  debug("TODO: check old_action is NULL");
}
}  // namespace mesh
