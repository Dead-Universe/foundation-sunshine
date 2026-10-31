#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "src/platform/windows/console_input_queue.h"
#include "../../../tests_common.h"
#include <atomic>
#include <thread>

using platf::console_input_bridge::input_queue;

TEST(ConsoleInputQueue, EmptyQueueDoesNotWaitForAReader) {
  input_queue queue;
  INPUT input {};
  EXPECT_FALSE(queue.pop(input));
}

TEST(ConsoleInputQueue, FullQueueFailsWithoutOverwritingPendingInput) {
  input_queue queue;
  for(ULONG i=0;i<input_queue::capacity;++i) {
    INPUT input {};input.type=INPUT_KEYBOARD;input.ki.dwExtraInfo=i;
    ASSERT_TRUE(queue.push(input));
  }
  INPUT input {};
  EXPECT_FALSE(queue.push(input));
  for(ULONG i=0;i<input_queue::capacity;++i) {
    ASSERT_TRUE(queue.pop(input));EXPECT_EQ(input.ki.dwExtraInfo,i);
  }
  EXPECT_FALSE(queue.pop(input));EXPECT_TRUE(queue.push(input));
}

TEST(ConsoleInputQueue, SequenceWrapPreservesOrderAndCapacity) {
  input_queue queue;
  queue.produced=queue.consumed=static_cast<LONG>(0xfffffffdu);
  for(ULONG i=0;i<8;++i) {
    INPUT input {};input.ki.dwExtraInfo=i;ASSERT_TRUE(queue.push(input));
  }
  for(ULONG i=0;i<8;++i) {
    INPUT input {};ASSERT_TRUE(queue.pop(input));EXPECT_EQ(input.ki.dwExtraInfo,i);
  }
  INPUT input {};EXPECT_FALSE(queue.pop(input));
}

TEST(ConsoleInputQueue, ConcurrentEventWakeupDrainsEveryPublishedInput) {
  input_queue queue;
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  ASSERT_NE(event,nullptr);
  constexpr ULONG count=20000;
  std::atomic<bool> stop {false},ordered {true};
  std::atomic<ULONG> received {0};
  const auto deadline=GetTickCount64()+10000;
  std::thread consumer([&] {
    ULONG expected=0;
    while(expected<count&&!stop.load()&&GetTickCount64()<deadline) {
      if(WaitForSingleObject(event,20)!=WAIT_OBJECT_0)continue;
      INPUT input {};
      while(queue.pop(input)) {
        if(input.ki.dwExtraInfo!=expected)ordered=false;
        ++expected;
      }
    }
    received=expected;
  });
  ULONG sent=0;
  for(;sent<count&&GetTickCount64()<deadline;++sent) {
    INPUT input {};input.type=INPUT_KEYBOARD;input.ki.dwExtraInfo=sent;
    while(!queue.push(input)) {
      if(GetTickCount64()>=deadline){stop=true;break;}
      std::this_thread::yield();
    }
    SetEvent(event);
    if(stop.load())break;
  }
  consumer.join();CloseHandle(event);
  EXPECT_EQ(sent,count);EXPECT_EQ(received.load(),count);EXPECT_TRUE(ordered.load());
}
#endif
