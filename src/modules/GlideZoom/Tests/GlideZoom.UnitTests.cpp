#include "PortableTestAdapter.h"

int Run_capture();
int Run_control_channel();
int Run_core();
int Run_cursor_visibility();
int Run_fusion(int, char**);
int Run_gpu_cursor_pixels();
int Run_gpu_frame_policy();
int Run_gpu_queue_policy();
int Run_input_context();
int Run_input_state();
int Run_input_transport();
int Run_latency();
int Run_monitor_geometry();
int Run_monitor_presenter();
int Run_overlay_order();
int Run_passive_monitor();
int Run_pointer_channel();
int Run_pointer_feedback();
int Run_pointer_motion();
int Run_settings();
int Run_shortcut();
int Run_touchpad_packets();
int Run_touchpad();
int Run_tray();
int Run_view_mode();
int Run_viewport_motion();
int Run_zoom_precision();
int Run_zoom_session();

namespace GlideZoomTests
{
    TEST_CLASS (EngineTests)
    {
    public:
        TEST_METHOD (capture)
        {
            RunPortableSuite([] { return Run_capture(); });
        }

        TEST_METHOD (control_channel)
        {
            RunPortableSuite([] { return Run_control_channel(); });
        }

        TEST_METHOD (core)
        {
            RunPortableSuite([] { return Run_core(); });
        }

        TEST_METHOD (cursor_visibility)
        {
            RunPortableSuite([] { return Run_cursor_visibility(); });
        }

        TEST_METHOD (fusion)
        {
            RunPortableSuite([] { return Run_fusion(0, nullptr); });
        }

        TEST_METHOD (gpu_cursor_pixels)
        {
            RunPortableSuite([] { return Run_gpu_cursor_pixels(); });
        }

        TEST_METHOD (gpu_frame_policy)
        {
            RunPortableSuite([] { return Run_gpu_frame_policy(); });
        }

        TEST_METHOD (gpu_queue_policy)
        {
            RunPortableSuite([] { return Run_gpu_queue_policy(); });
        }

        TEST_METHOD (input_context)
        {
            RunPortableSuite([] { return Run_input_context(); });
        }

        TEST_METHOD (input_state)
        {
            RunPortableSuite([] { return Run_input_state(); });
        }

        TEST_METHOD (input_transport)
        {
            RunPortableSuite([] { return Run_input_transport(); });
        }

        TEST_METHOD (latency)
        {
            RunPortableSuite([] { return Run_latency(); });
        }

        TEST_METHOD (monitor_geometry)
        {
            RunPortableSuite([] { return Run_monitor_geometry(); });
        }

        TEST_METHOD (monitor_presenter)
        {
            RunPortableSuite([] { return Run_monitor_presenter(); });
        }

        TEST_METHOD (overlay_order)
        {
            RunPortableSuite([] { return Run_overlay_order(); });
        }

        TEST_METHOD (passive_monitor)
        {
            RunPortableSuite([] { return Run_passive_monitor(); });
        }

        TEST_METHOD (pointer_channel)
        {
            RunPortableSuite([] { return Run_pointer_channel(); });
        }

        TEST_METHOD (pointer_feedback)
        {
            RunPortableSuite([] { return Run_pointer_feedback(); });
        }

        TEST_METHOD (pointer_motion)
        {
            RunPortableSuite([] { return Run_pointer_motion(); });
        }

        TEST_METHOD (settings)
        {
            RunPortableSuite([] { return Run_settings(); });
        }

        TEST_METHOD (shortcut)
        {
            RunPortableSuite([] { return Run_shortcut(); });
        }

        TEST_METHOD (touchpad_packets)
        {
            RunPortableSuite([] { return Run_touchpad_packets(); });
        }

        TEST_METHOD (touchpad)
        {
            RunPortableSuite([] { return Run_touchpad(); });
        }

        TEST_METHOD (tray)
        {
            RunPortableSuite([] { return Run_tray(); });
        }

        TEST_METHOD (view_mode)
        {
            RunPortableSuite([] { return Run_view_mode(); });
        }

        TEST_METHOD (viewport_motion)
        {
            RunPortableSuite([] { return Run_viewport_motion(); });
        }

        TEST_METHOD (zoom_precision)
        {
            RunPortableSuite([] { return Run_zoom_precision(); });
        }

        TEST_METHOD (zoom_session)
        {
            RunPortableSuite([] { return Run_zoom_session(); });
        }
    };
}
