#ifndef __LIBRECOMP_STARTUP_SHUTDOWN_HPP__
#define __LIBRECOMP_STARTUP_SHUTDOWN_HPP__

#include <atomic>

namespace recomp {
    // [wcw fix] jessetbh/aki-roadmap#73. A quit (WCW_QUIT_AFTER, or closing the window
    // before a game is picked) can land while the game start thread is still in
    // ultramodern::preinit, which waits for the gfx thread to create the renderer. On
    // Windows that creation touches the main thread's window (RT64's
    // ApplicationWindow::setFullScreen calls SetWindowLongPtr), which only completes once
    // the main thread processes window messages. recomp::start used to stop pumping as
    // soon as `exited` was set and then block joining the game thread: a deadlock.
    //
    // Keep calling pump() until startup_done is true, then return. Shutdown proceeds as
    // before afterwards, so a quit after startup behaves exactly as it used to.
    template <typename Pump, typename Sleep>
    void pump_until_startup_done(const std::atomic_bool& startup_done, Pump&& pump, Sleep&& sleep) {
        while (!startup_done.load()) {
            sleep();
            pump();
        }
    }
}

#endif
