/* SPDX-License-Identifier: Apache-2.0 */
// Reuse the manager fixture and lifecycle checks with the real Linux driver policy.
#include "test_hrm_manager.c"

void test_hrm_manager_linux__initialize(void) {
  test_hrm_manager__initialize();
}

void test_hrm_manager_linux__bpm_lifecycle_without_host_accel(void) {
  test_hrm_manager__subscription();
  cl_assert(s_accel_data_cb == nullptr);
}

void test_hrm_manager_linux__bpm_delivery_without_host_accel(void) {
  test_hrm_manager__system_task_data_callback();
  cl_assert(s_accel_data_cb == nullptr);
}

void test_hrm_manager_linux__unserved_timeout_without_host_accel(void) {
  test_hrm_manager__unserved_subscriber_timeout();
  cl_assert(s_accel_data_cb == nullptr);
}

void test_hrm_manager_linux__off_wrist_without_host_accel(void) {
  test_hrm_manager__immediate_off_wrist();
  cl_assert(s_accel_data_cb == nullptr);
}

// The vendor service has no SpO2 path: such a subscriber must never try to enable the sensor.
void test_hrm_manager_linux__spo2_never_enables_sensor(void) {
  s_activity_prefs_spo2_is_enabled = true;
  HRMSessionRef session_ref = sys_hrm_manager_app_subscribe(1 /*app_id*/, 1 /*interval_s*/,
                                                            0 /*expire_s*/, HRMFeature_SpO2);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), false);
  cl_assert_equal_i(s_hrm_state.enable_count, 0);

  sys_hrm_manager_unsubscribe(session_ref);
}
