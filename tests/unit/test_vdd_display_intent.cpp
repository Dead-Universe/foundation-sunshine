#ifdef _WIN32
#include <src/config.h>
#include <src/display_device/parsed_config.h>
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
#endif
