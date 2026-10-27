#ifdef _WIN32
#include <src/nvenc/win/impl/nvenc_async_wait.h>
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

TEST(NvencAsyncCompletion, SignaledEventDoesNotEnterGraceInterval) {
  HANDLE event = CreateEventW(nullptr, FALSE, TRUE, nullptr);
  ASSERT_NE(event, nullptr);
  const auto result = nvenc::async_completion::wait(event, 100);
  CloseHandle(event);
  EXPECT_EQ(result.status, WAIT_OBJECT_0);
  EXPECT_FALSE(result.extended);
}

TEST(NvencAsyncCompletion, LateCompletionKeepsTheSubmittedFrame) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  ASSERT_NE(event, nullptr);
  std::jthread producer([event] {
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    SetEvent(event);
  });
  const auto result = nvenc::async_completion::wait(event, 100);
  producer.join();
  CloseHandle(event);
  EXPECT_EQ(result.status, WAIT_OBJECT_0);
  EXPECT_TRUE(result.extended);
}

TEST(NvencAsyncCompletion, MissingCompletionStillHasABoundedFailure) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  ASSERT_NE(event, nullptr);
  const auto result = nvenc::async_completion::wait(event, 100);
  CloseHandle(event);
  EXPECT_EQ(result.status, WAIT_TIMEOUT);
  EXPECT_TRUE(result.extended);
}

TEST(NvencAsyncCompletion, InvalidHandleFailsWithoutGraceInterval) {
  const auto result = nvenc::async_completion::wait(nullptr, 100);
  EXPECT_EQ(result.status, WAIT_FAILED);
  EXPECT_EQ(result.error, ERROR_INVALID_HANDLE);
  EXPECT_FALSE(result.extended);
}
#endif
