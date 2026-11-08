#include "PortableTestAdapter.h"

int Run_touchpad_hid();

namespace GlideZoomTests
{
    TEST_CLASS(EngineTests)
    {
    public:
        TEST_METHOD(touchpad_hid)
        {
            RunPortableSuite([] { return Run_touchpad_hid(); });
        }

    };
}
