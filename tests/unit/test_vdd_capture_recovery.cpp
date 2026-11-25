#include <src/vdd_capture_recovery.h>

#include <gtest/gtest.h>
#include <stdexcept>

TEST(VddCaptureRecovery, FailedRecoveryRestoresEmptyAuto) {
  std::string capture;
  { video::vdd_capture_recovery_t recovery {capture}; EXPECT_EQ(capture, "vdd"); }
  EXPECT_TRUE(capture.empty());
}

TEST(VddCaptureRecovery, FailedRecoveryPreservesLiteralAuto) {
  std::string capture = "auto";
  { video::vdd_capture_recovery_t recovery {capture}; EXPECT_EQ(capture, "vdd"); }
  EXPECT_EQ(capture, "auto");
}

TEST(VddCaptureRecovery, SuccessfulRecoveryKeepsTheValidatedBackend) {
  std::string capture;
  { video::vdd_capture_recovery_t recovery {capture}; recovery.commit(); }
  EXPECT_EQ(capture, "vdd");
}

TEST(VddCaptureRecovery, ExplicitCaptureChoicesArePreserved) {
  for (const auto *backend : {"ddx", "wgc", "vdd"}) {
    std::string capture = backend;
    { video::vdd_capture_recovery_t recovery {capture}; EXPECT_EQ(capture, backend); recovery.commit(); }
    EXPECT_EQ(capture, backend);
  }
}

TEST(VddCaptureRecovery, ExceptionRestoresTheOriginalPreference) {
  std::string capture = "auto";
  try {
    video::vdd_capture_recovery_t recovery {capture};
    throw std::runtime_error("simulated display preparation failure");
  } catch (const std::runtime_error &) { }
  EXPECT_EQ(capture, "auto");
}

TEST(VddCaptureRecovery, ActiveCaptureDoesNotChangeThePreference) {
  std::string capture;
  { video::vdd_capture_recovery_t recovery {capture, false}; EXPECT_TRUE(capture.empty()); recovery.commit(); }
  EXPECT_TRUE(capture.empty());
}
