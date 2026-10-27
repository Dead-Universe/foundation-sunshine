#ifdef _WIN32
#include <src/config.h>
#include <src/display_device/parsed_config.h>
#include <src/display_device/vdd_output_state.h>
#include <src/globals.h>
#include <src/rtsp.h>
#include <gtest/gtest.h>

TEST(VddDisplayIntent, AutomaticOutputFollowsTheResolvedVddBackend) {
  auto config = config::video;
  config.capture = "vdd";
  config.output_name.clear();
  rtsp_stream::launch_session_t session {};
  const auto intent = display_device::resolve_display_intent(config, session);
  EXPECT_EQ(intent.target, display_device::display_intent_t::target_e::vdd);
  EXPECT_FALSE(intent.user_named_display);
}

TEST(VddDisplayIntent, UnresolvedAutoKeepsTheUpstreamPhysicalDefault) {
  auto config = config::video;
  config.capture.clear();
  config.output_name.clear();
  rtsp_stream::launch_session_t session {};
  EXPECT_EQ(display_device::resolve_display_intent(config, session).target,
            display_device::display_intent_t::target_e::physical);
}

TEST(VddDisplayIntent, ClientDisplaySelectionIsNeverImplicitlyReplacedByVdd) {
  auto config = config::video;
  config.capture = "vdd";
  config.output_name.clear();
  rtsp_stream::launch_session_t session {};
  session.env["SUNSHINE_CLIENT_DISPLAY_NAME"] = "nonexistent-client-physical-display";
  const auto intent = display_device::resolve_display_intent(config, session);
  EXPECT_NE(intent.target, display_device::display_intent_t::target_e::vdd);
  EXPECT_TRUE(intent.user_named_display);
  EXPECT_EQ(intent.device_id, "nonexistent-client-physical-display");
}

TEST(VddDisplayIntent, HostDisplaySelectionKeepsTheUpstreamResolutionPolicy) {
  auto config = config::video;
  config.capture = "vdd";
  config.output_name = "nonexistent-host-physical-display";
  rtsp_stream::launch_session_t session {};
  EXPECT_NE(display_device::resolve_display_intent(config, session).target,
            display_device::display_intent_t::target_e::vdd);
}

TEST(VddDisplayIntent, ClientCanStillExplicitlyRequestTheVddAlias) {
  auto config = config::video;
  config.capture.clear();
  config.output_name = "physical-display";
  rtsp_stream::launch_session_t session {};
  session.env["SUNSHINE_CLIENT_DISPLAY_NAME"] = VDD_NAME;
  EXPECT_EQ(display_device::resolve_display_intent(config, session).target,
            display_device::display_intent_t::target_e::vdd);
}
TEST(VddDisplayIntent, AutoOutputIsRestoredForTheNextVddSession) {
  auto config = config::video;
  config.capture = "vdd";
  config.output_name.clear();
  display_device::vdd_output_state_t original;
  ASSERT_TRUE(original.remember(config.output_name));
  config.output_name = "destroyed-vdd-device-id";

  ASSERT_TRUE(original.restore(config.output_name));
  EXPECT_TRUE(config.output_name.empty());
  const auto next = display_device::resolve_display_intent(config, {});
  EXPECT_EQ(next.target, display_device::display_intent_t::target_e::vdd);
  EXPECT_FALSE(next.user_named_display);
}

TEST(VddOutputState, ReconfigurationPreservesTheFirstEmptyAutoSelection) {
  display_device::vdd_output_state_t original;
  std::string output;
  ASSERT_TRUE(original.remember(output));
  output = "first-vdd";
  EXPECT_FALSE(original.remember(output));
  output = "replacement-vdd";
  ASSERT_TRUE(original.restore(output));
  EXPECT_TRUE(output.empty());
}

TEST(VddOutputState, RestoresExplicitSelectionAndConsumesTheSnapshot) {
  display_device::vdd_output_state_t original;
  std::string output = "user-selected-physical-display";
  ASSERT_TRUE(original.remember(output));
  output = "temporary-vdd";
  ASSERT_TRUE(original.restore(output));
  EXPECT_EQ(output, "user-selected-physical-display");

  output = "later-user-selection";
  EXPECT_FALSE(original.restore(output));
  EXPECT_EQ(output, "later-user-selection");
}

TEST(VddOutputState, ANewSessionCanSaveAutoAfterAnExplicitSelection) {
  display_device::vdd_output_state_t original;
  std::string output = "first-user-selection";
  ASSERT_TRUE(original.remember(output));
  output = "first-vdd";
  ASSERT_TRUE(original.restore(output));

  output.clear();
  ASSERT_TRUE(original.remember(output));
  output = "second-vdd";
  ASSERT_TRUE(original.restore(output));
  EXPECT_TRUE(output.empty());
}

#endif
