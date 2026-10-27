/**
 * @file tests/unit/test_sunshinesvc_state.cpp
 * @brief Tests for the GUI agent lifecycle policy used by sunshinesvc.
 */
#include <tools/sunshinesvc_state.h>

#include <array>
#include <gtest/gtest.h>

TEST(GuiRestartBackoff, BacksOffRepeatedFailures) {
  sunshinesvc::GuiRestartBackoff backoff;
  constexpr std::array expected { 1000U, 2000U, 4000U, 8000U, 16000U, 30000U, 30000U };

  for (std::size_t index = 0; index < expected.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_EQ(backoff.next_delay(), expected[index]);
  }
}

TEST(GuiRestartBackoff, ResetsOnlyAfterStableRuntime) {
  sunshinesvc::GuiRestartBackoff backoff;

  EXPECT_EQ(backoff.next_delay(), 1000U);
  EXPECT_EQ(backoff.next_delay(), 2000U);
  EXPECT_EQ(backoff.next_delay(sunshinesvc::GUI_RESTART_STABLE_RUNTIME_MS - 1), 4000U);
  EXPECT_EQ(backoff.next_delay(sunshinesvc::GUI_RESTART_STABLE_RUNTIME_MS), 1000U);
  EXPECT_EQ(backoff.next_delay(), 2000U);
}

TEST(GuiRestartBackoff, ExplicitResetStartsAtInitialDelay) {
  sunshinesvc::GuiRestartBackoff backoff;

  EXPECT_EQ(backoff.next_delay(), 1000U);
  EXPECT_EQ(backoff.next_delay(), 2000U);
  backoff.reset();
  EXPECT_EQ(backoff.next_delay(), 1000U);
}

TEST(GuiAgentRestartPolicy, CleanExitSuppressesLaunchUntilSupervisionResumes) {
  sunshinesvc::GuiAgentRestartPolicy policy;

  EXPECT_TRUE(policy.launch_allowed());
  policy.suppress_launch();
  EXPECT_FALSE(policy.launch_allowed());
  policy.resume_supervision();
  EXPECT_TRUE(policy.launch_allowed());
}

TEST(GuiAgentCrashLogLimiter, LogsStateChangesAndPeriodicReminders) {
  sunshinesvc::GuiAgentCrashLogLimiter limiter;

  EXPECT_TRUE(limiter.should_log_acquisition(false));
  EXPECT_FALSE(limiter.should_log_acquisition(false));

  EXPECT_TRUE(limiter.should_log_exit(1, 1000, 0));
  EXPECT_TRUE(limiter.should_log_acquisition(false));
  EXPECT_FALSE(limiter.should_log_exit(1, 1000, sunshinesvc::GUI_CRASH_LOG_REMINDER_MS - 1));
  EXPECT_FALSE(limiter.should_log_acquisition(false));
  EXPECT_TRUE(limiter.should_log_exit(1, 1000, sunshinesvc::GUI_CRASH_LOG_REMINDER_MS));
  EXPECT_TRUE(limiter.should_log_acquisition(false));

  EXPECT_TRUE(limiter.should_log_exit(1, 2000, sunshinesvc::GUI_CRASH_LOG_REMINDER_MS + 1));
  EXPECT_TRUE(limiter.should_log_exit(2, 2000, sunshinesvc::GUI_CRASH_LOG_REMINDER_MS + 2));
  EXPECT_TRUE(limiter.should_log_acquisition(false));
  EXPECT_TRUE(limiter.should_log_acquisition(true));

  limiter.reset();
  EXPECT_TRUE(limiter.should_log_acquisition(false));
}

TEST(ConsoleSessionAction, SealedVddSurvivesCapturedConsoleRoundTrip) {
  using A = sunshinesvc::ConsoleSessionAction;
  for (auto target : std::array<std::uint32_t, 5>{1, UINT32_MAX, 4, UINT32_MAX, 1}) {
    EXPECT_NE(sunshinesvc::console_session_action(1, target, true), A::restart_core);
  }
}

TEST(ConsoleSessionAction, StandardCaptureStillMovesToAnotherSession) {
  using A = sunshinesvc::ConsoleSessionAction;
  EXPECT_EQ(sunshinesvc::console_session_action(1, 1, false), A::keep_core);
  EXPECT_EQ(sunshinesvc::console_session_action(1, 4, false), A::restart_core);
}

TEST(ConsoleSessionAction, ServiceCoreSurvivesAnonymousSessionRetirementEvenWhileIdle) {
  using A = sunshinesvc::ConsoleSessionAction;
  // Captured terminal-session trace: the anonymous console is retired when
  // credentials reconnect the existing RDP user. Core must already be in 0.
  for (bool media_ready : {false, true}) {
    for (auto target : std::array<std::uint32_t, 5>{2, UINT32_MAX, 4, UINT32_MAX, 2}) {
      EXPECT_NE(sunshinesvc::console_session_action(0, target, media_ready, true), A::restart_core);
    }
  }
}

TEST(ConsoleSessionAction, PersistentPolicyCannotMakeAnInteractiveCoreDurable) {
  using A = sunshinesvc::ConsoleSessionAction;
  EXPECT_EQ(sunshinesvc::console_session_action(4, 2, false, true), A::restart_core);
  EXPECT_EQ(sunshinesvc::console_session_action(0, 2, false, false), A::restart_core);
}

TEST(ConsoleSessionAction, InvalidConsoleNeverTerminatesEitherCaptureMode) {
  using A = sunshinesvc::ConsoleSessionAction;
  EXPECT_EQ(sunshinesvc::console_session_action(1, UINT32_MAX, false), A::probe_again);
  EXPECT_EQ(sunshinesvc::console_session_action(1, UINT32_MAX, true), A::probe_again);
}
TEST(ConsoleTransportReadiness, EncoderPreflightDoesNotPinAnIdleCore) {
  using A = sunshinesvc::ConsoleSessionAction;
  const bool preflight_only = sunshinesvc::should_retain_transport(false, true, true);
  EXPECT_FALSE(preflight_only);
  EXPECT_EQ(sunshinesvc::console_session_action(1, 3, preflight_only), A::restart_core);
  EXPECT_FALSE(sunshinesvc::should_retain_transport(true, false, true));
  EXPECT_FALSE(sunshinesvc::should_retain_transport(true, true, false));
}

TEST(ConsoleTransportReadiness, ActiveLoginStreamSurvivesUntilItsLastMediaSessionStops) {
  using A = sunshinesvc::ConsoleSessionAction;
  const bool streaming = sunshinesvc::should_retain_transport(true, true, true);
  EXPECT_TRUE(streaming);
  EXPECT_EQ(sunshinesvc::console_session_action(3, UINT32_MAX, streaming), A::probe_again);
  EXPECT_EQ(sunshinesvc::console_session_action(3, 1, streaming), A::keep_core);
  const bool stopped = sunshinesvc::should_retain_transport(false, true, true);
  EXPECT_EQ(sunshinesvc::console_session_action(3, 1, stopped), A::restart_core);
}
