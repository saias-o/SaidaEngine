#include "core/InputTouch.hpp"

#include <cassert>

int main() {
    using saida::TouchGesture;
    using namespace saida::input_detail;

    assert(classifyTouchGesture({10.0f, 10.0f}, {20.0f, 20.0f}) ==
           TouchGesture::Tap);
    assert(classifyTouchGesture({0.0f, 0.0f}, {100.0f, 10.0f}) ==
           TouchGesture::SwipeRight);
    assert(classifyTouchGesture({100.0f, 0.0f}, {0.0f, 10.0f}) ==
           TouchGesture::SwipeLeft);
    assert(classifyTouchGesture({0.0f, 100.0f}, {10.0f, 0.0f}) ==
           TouchGesture::SwipeUp);
    assert(classifyTouchGesture({0.0f, 0.0f}, {10.0f, 100.0f}) ==
           TouchGesture::SwipeDown);

    assert(touchPointInZone({750.0f, 250.0f}, {1000.0f, 500.0f},
                            {0.5f, 0.0f}, {1.0f, 1.0f}));
    assert(!touchPointInZone({250.0f, 250.0f}, {1000.0f, 500.0f},
                             {0.5f, 0.0f}, {1.0f, 1.0f}));
    assert(!touchPointInZone({0.0f, 0.0f}, {0.0f, 0.0f},
                             {0.0f, 0.0f}, {1.0f, 1.0f}));
    assert(validTouchZone({0.0f, 0.25f}, {0.5f, 1.0f}));
    assert(!validTouchZone({0.8f, 0.0f}, {0.2f, 1.0f}));
    return 0;
}
